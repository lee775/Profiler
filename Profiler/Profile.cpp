#include "Profile.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <cstdio>
#include <cstring>
#include <cfloat>
#include <new>			// std::nothrow

namespace
{
	constexpr int MAX_PROFILE_TAGS = 32;			// 스레드당 등록 가능한 태그 수
	constexpr int MAX_TAG_LEN = 40;					// 태그 이름 최대 길이(널 포함). 보고서 Name 컬럼 폭과 동일하게 유지한다
	constexpr int MAX_ERRORS = 32;					// 보고서에 남길 오류 수
	constexpr int ERROR_MSG_LEN = 160;				// thread id + 메시지 + 태그가 잘리지 않을 크기
	constexpr long long CALLS_FOR_OUTLIER_CUT = 5;	// 상·하위 2개씩 제외하려면 최소 5회 필요

	// 보고서 컬럼 폭. 구분선/헤더/데이터 행이 모두 이 상수들로부터 만들어지므로
	// 한 곳만 바꿔도 표 전체가 어긋나지 않는다.
	constexpr int W_TID = 10;						// DWORD thread id 최대 10자리
	constexpr int W_NAME = MAX_TAG_LEN - 1;			// 저장 폭 == 표시 폭 → 잘림으로 인한 태그 혼동이 없다
	constexpr int W_NUM = 16;						// 11 정수부 + '.' + 4 소수 → 1000초(≈16분) 넘는 구간도 정렬 유지
	constexpr int W_CALL = 12;

	const char DEFAULT_FILE_NAME[] = "profile.txt";

	struct ProfileSample
	{
		char tag[MAX_TAG_LEN];	// 호출자 포인터를 저장하지 않고 복사해 보관한다
		long long startTime;	// QPC 틱
		double totalTime;		// 누적 시간(us)
		double min[2];			// min[0]=최소, min[1]=두 번째 최소
		double max[2];			// max[0]=최대, max[1]=두 번째 최대
		long long call;			// 완료된 측정 횟수(END에서 증가)
		bool active;			// BEGIN~END 사이 여부
	};

	// 스레드마다 하나씩 힙에 만들어져 레지스트리에 등록된다.
	// 스레드가 종료해도 블록은 프로세스가 끝날 때까지 남으므로 결과가 유실되지 않는다.
	struct ThreadBlock
	{
		DWORD threadId;
		SRWLOCK lock;			// slots/used 보호. 소유 스레드가 짧게 배타 획득, 출력/리셋만 외부에서 접근한다
		int used;
		ProfileSample slots[MAX_PROFILE_TAGS];
		ThreadBlock* next;
	};

	// 락 계층(항상 이 순서로만 중첩 획득 → 데드락 방지):
	//   g_printLock > g_registryLock > ThreadBlock::lock > g_errorLock
	// Begin/End는 block lock만, 그리고 그 락을 푼 뒤에만 LogError(g_errorLock)를 호출한다.
	SRWLOCK g_printLock = SRWLOCK_INIT;		// 모든 보고서 출력(자동/수동)을 직렬화한다
	SRWLOCK g_registryLock = SRWLOCK_INIT;
	ThreadBlock* g_blockHead = nullptr;

	SRWLOCK g_errorLock = SRWLOCK_INIT;
	char g_errors[MAX_ERRORS][ERROR_MSG_LEN];
	int g_errorCount = 0;			// 실제로 버퍼에 담긴 개수(<= MAX_ERRORS)
	long long g_errorTotal = 0;		// 발생한 전체 오류 수(버려진 것 포함) → 오탐 없는 생략 안내에 사용

	long long QpcFrequency()
	{
		// QPC 주파수는 부팅 후 불변이므로 최초 1회만 조회한다
		static const long long freq = []
		{
			LARGE_INTEGER f;
			QueryPerformanceFrequency(&f);
			return f.QuadPart;
		}();
		return freq;
	}

	// 디버거가 붙어 있을 때만 브레이크한다. 검사와 브레이크 사이에 디버거가 분리되어도
	// 처리되지 않은 STATUS_BREAKPOINT로 프로세스가 죽지 않도록 SEH로 감싼다.
	void SafeDebugBreak()
	{
		if (!IsDebuggerPresent())
			return;
		__try
		{
			DebugBreak();
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			// 브레이크 시점에 디버거가 사라졌다 → 무시하고 계속 진행
		}
	}

	// 오류는 프로세스를 죽이는 대신 기록해 두었다가 보고서 하단에 출력한다.
	// thread id를 먼저 써서, 긴 태그로 뒷부분이 잘려도 어느 스레드인지는 항상 남게 한다.
	void LogError(const char* what, const char* tag)
	{
		AcquireSRWLockExclusive(&g_errorLock);
		g_errorTotal++;
		if (g_errorCount < MAX_ERRORS)
		{
			_snprintf_s(g_errors[g_errorCount], sizeof(g_errors[0]), _TRUNCATE,
				"[thread %lu] %s : %s", GetCurrentThreadId(), what, tag);
			g_errorCount++;
		}
		ReleaseSRWLockExclusive(&g_errorLock);

		SafeDebugBreak();
	}

	// 실패 시 nullptr을 반환한다(예외를 던지지 않는다). 프로파일링은 계측 코드이므로
	// 메모리 부족으로 호출자 코드에 예외를 전파해 프로세스를 흔드는 것보다 이번 호출을 건너뛰는 편이 안전하다.
	ThreadBlock* CreateThreadBlock()
	{
		ThreadBlock* block = new (std::nothrow) ThreadBlock{};
		if (block == nullptr)
			return nullptr;
		block->threadId = GetCurrentThreadId();
		InitializeSRWLock(&block->lock);

		AcquireSRWLockExclusive(&g_registryLock);
		block->next = g_blockHead;
		g_blockHead = block;
		ReleaseSRWLockExclusive(&g_registryLock);
		return block;
	}

	// 이 스레드의 블록을 반환한다. 반환값이 nullptr이면 이번 호출은 프로파일링을 건너뛴다.
	// 포인터는 0으로 상수 초기화되어 동적 초기화 가드가 없으므로, 할당 경로를
	// 프로파일링(예: operator new 후킹)하더라도 재진입 시 무한 재귀에 빠지지 않는다.
	// 할당이 실패해도 tlsInInit을 반드시 되돌리므로, 일시적 OOM이 그 스레드의 프로파일링을 영구히 끄지 않는다.
	ThreadBlock* GetThreadBlock()
	{
		static thread_local ThreadBlock* tlsBlock = nullptr;
		static thread_local bool tlsInInit = false;

		if (tlsBlock != nullptr)
			return tlsBlock;
		if (tlsInInit)
			return nullptr;			// 자기 자신의 블록 생성 도중 재진입 → 이번 호출은 포기

		tlsInInit = true;
		tlsBlock = CreateThreadBlock();
		tlsInInit = false;
		return tlsBlock;			// 실패했으면 여전히 nullptr → 다음 호출에서 다시 시도한다
	}

	// block->lock을 잡은 상태에서 호출할 것.
	// 저장 시 W_NAME자로 잘리므로 비교도 같은 길이로 해야 긴 태그의 BEGIN/END가 짝을 찾는다
	ProfileSample* FindSlot(ThreadBlock* block, const char* tag)
	{
		for (int i = 0; i < block->used; i++)
		{
			if (strncmp(block->slots[i].tag, tag, W_NAME) == 0)
				return &block->slots[i];
		}
		return nullptr;
	}

	// 프로세스 정상 종료 시 결과를 자동 출력한다(측정이 하나라도 있었던 경우에만).
	// 주의: 다른 번역 단위의 정적 소멸자에서 수행되는 측정은 링크 순서에 따라 이 시점 이후에
	// 일어날 수 있어 자동 출력에 포함되지 않을 수 있다. 종료 구간을 측정하려면 명시적으로 PRO_PRINT()할 것.
	struct AtExitPrinter
	{
		~AtExitPrinter()
		{
			AcquireSRWLockShared(&g_registryLock);
			const bool any = (g_blockHead != nullptr);
			ReleaseSRWLockShared(&g_registryLock);

			if (any)
				ProfileDataOutText(DEFAULT_FILE_NAME);
		}
	} g_atExitPrinter;
}

void ProfileBegin(const char* tag)
{
	if (tag == nullptr)
		tag = "(null)";			// 널 태그로 프로세스를 죽이지 않는다("오사용은 기록하되 중단 없음" 원칙)

	ThreadBlock* block = GetThreadBlock();
	if (block == nullptr)
		return;
	AcquireSRWLockExclusive(&block->lock);

	ProfileSample* slot = FindSlot(block, tag);
	if (slot == nullptr)
	{
		if (block->used >= MAX_PROFILE_TAGS)
		{
			ReleaseSRWLockExclusive(&block->lock);
			LogError("BEGIN ignored: tag capacity exceeded", tag);
			return;
		}
		slot = &block->slots[block->used++];
		strncpy_s(slot->tag, tag, _TRUNCATE);
		slot->totalTime = 0.0;
		slot->min[0] = DBL_MAX;
		slot->min[1] = DBL_MAX;
		slot->max[0] = 0.0;
		slot->max[1] = 0.0;
		slot->call = 0;
		slot->active = false;
	}

	const bool wasActive = slot->active;
	slot->active = true;

	// 시작 시각은 마지막에 캡처해 프로파일러 자체 오버헤드를 측정에서 배제한다
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	slot->startTime = now.QuadPart;

	ReleaseSRWLockExclusive(&block->lock);

	if (wasActive)
	{
		// 연속 BEGIN: 이전 구간은 버리고 이번 구간으로 다시 시작한다(락 밖에서 기록)
		LogError("Consecutive BEGIN, previous interval discarded", tag);
	}
}

void ProfileEnd(const char* tag)
{
	// 종료 시각을 가장 먼저 캡처해 탐색/락 비용을 측정에서 배제한다
	LARGE_INTEGER endTime;
	QueryPerformanceCounter(&endTime);

	if (tag == nullptr)
		tag = "(null)";

	ThreadBlock* block = GetThreadBlock();
	if (block == nullptr)
		return;
	AcquireSRWLockExclusive(&block->lock);

	ProfileSample* slot = FindSlot(block, tag);
	if (slot == nullptr)
	{
		ReleaseSRWLockExclusive(&block->lock);
		LogError("END ignored: unknown tag", tag);
		return;
	}
	if (!slot->active)
	{
		ReleaseSRWLockExclusive(&block->lock);
		LogError("END ignored: no matching BEGIN", tag);
		return;
	}

	const double elapsed =
		(double)(endTime.QuadPart - slot->startTime) * 1000000.0 / (double)QpcFrequency();

	slot->totalTime += elapsed;
	slot->call++;

	// 상위 2개 유지: 1위 갱신 시 기존 1위를 2위로 내린다
	if (elapsed > slot->max[0])
	{
		slot->max[1] = slot->max[0];
		slot->max[0] = elapsed;
	}
	else if (elapsed > slot->max[1])
	{
		slot->max[1] = elapsed;
	}

	// 하위 2개 유지: 동일하게 시프트 삽입
	if (elapsed < slot->min[0])
	{
		slot->min[1] = slot->min[0];
		slot->min[0] = elapsed;
	}
	else if (elapsed < slot->min[1])
	{
		slot->min[1] = elapsed;
	}

	slot->active = false;
	ReleaseSRWLockExclusive(&block->lock);
}

void ProfileDataOutText(const char* szFileName)
{
	if (szFileName == nullptr)
		szFileName = DEFAULT_FILE_NAME;

	// 모든 출력을 직렬화한다: 자동 출력과 여러 스레드의 PRO_PRINT가 겹쳐도
	// 같은 파일을 동시에 열어 한쪽 리포트가 통째로 유실되는 일이 없다.
	AcquireSRWLockExclusive(&g_printLock);

	// 임시 파일에 먼저 쓰고 원자적으로 교체한다. 출력 도중 크래시/전원 차단이 나도
	// 직전의 정상 리포트가 파괴되지 않는다(주기적 PRO_PRINT로 중간 결과를 보존하는 용도에 필요).
	char tmpName[512];
	_snprintf_s(tmpName, sizeof(tmpName), _TRUNCATE, "%s.tmp", szFileName);

	FILE* file = nullptr;
	if (fopen_s(&file, tmpName, "wt") != 0 || file == nullptr)
	{
		fprintf(stderr, "Profile: failed to open output file '%s'\n", tmpName);
		ReleaseSRWLockExclusive(&g_printLock);
		return;
	}

	// 헤더/데이터/구분선을 같은 폭 상수로 만들어 표가 어긋나지 않게 한다
	// (일반적인 값 기준. 극단적으로 큰 값은 최소폭을 넘어설 수 있다.)
	char dash[W_TID + 3 + W_NAME + 3 + W_NUM + 3 + W_NUM + 3 + W_NUM + 3 + W_CALL + 1];
	memset(dash, '-', sizeof(dash) - 1);
	dash[sizeof(dash) - 1] = '\0';

	fprintf(file, "%s\n", dash);
	fprintf(file, "%*s | %-*s | %*s | %*s | %*s | %*s\n",
		W_TID, "ThreadId", W_NAME, "Name",
		W_NUM, "Average(us)", W_NUM, "Min(us)", W_NUM, "Max(us)", W_CALL, "Call");
	fprintf(file, "%s\n", dash);

	// 블록 리스트는 앞쪽 삽입만 일어나므로 head 스냅숏 이후에는 락 없이 따라가도 안전하다
	AcquireSRWLockShared(&g_registryLock);
	ThreadBlock* head = g_blockHead;
	ReleaseSRWLockShared(&g_registryLock);

	for (ThreadBlock* block = head; block != nullptr; block = block->next)
	{
		// 블록 락은 슬롯을 로컬로 복사하는 동안만 아주 짧게 잡는다.
		// 파일 I/O는 락을 푼 뒤에 수행 → 소유 스레드의 BEGIN/END가 디스크 쓰기에 막히지 않는다.
		ProfileSample local[MAX_PROFILE_TAGS];
		DWORD threadId;
		int used;

		AcquireSRWLockShared(&block->lock);
		threadId = block->threadId;
		used = block->used;
		memcpy(local, block->slots, sizeof(ProfileSample) * (size_t)used);
		ReleaseSRWLockShared(&block->lock);

		for (int i = 0; i < used; i++)
		{
			const ProfileSample& s = local[i];

			double avg, mn, mx;
			if (s.call <= 0)
			{
				// 등록됐지만 완료된 측정이 없음(짝 없는 BEGIN이거나 첫 측정 진행 중).
				// 숨기지 않고 Call=0 행으로 드러내 진단 가능하게 한다.
				avg = 0.0; mn = 0.0; mx = 0.0;
			}
			else if (s.call >= CALLS_FOR_OUTLIER_CUT)
			{
				// 상·하위 2개씩 제외한 평균
				avg = (s.totalTime - s.max[0] - s.max[1] - s.min[0] - s.min[1])
					/ (double)(s.call - 4);
				mn = s.min[0];
				mx = s.max[0];
			}
			else
			{
				// 표본이 적으면 극값 제외 없이 단순 평균 (0으로 나누기/센티널 오염 방지)
				avg = s.totalTime / (double)s.call;
				mn = s.min[0];
				mx = s.max[0];
			}

			fprintf(file, "%*lu | %-*.*s | %*.*f | %*.*f | %*.*f | %*lld\n",
				W_TID, (unsigned long)threadId,
				W_NAME, W_NAME, s.tag,
				W_NUM, 4, avg, W_NUM, 4, mn, W_NUM, 4, mx,
				W_CALL, s.call);
		}
	}
	fprintf(file, "%s\n", dash);

	AcquireSRWLockShared(&g_errorLock);
	if (g_errorTotal > 0)
	{
		fprintf(file, "\n[Errors]\n");
		for (int i = 0; i < g_errorCount; i++)
			fprintf(file, "%s\n", g_errors[i]);
		if (g_errorTotal > MAX_ERRORS)
			fprintf(file, "(%lld errors total; %lld omitted beyond the %d-entry limit)\n",
				g_errorTotal, g_errorTotal - MAX_ERRORS, MAX_ERRORS);
	}
	ReleaseSRWLockShared(&g_errorLock);

	// 쓰기 오류를 확인한다. fclose가 남은 버퍼를 flush하므로 그 결과도 중요하다.
	// 디스크 꽉 참(ENOSPC) 등으로 임시 파일이 부분 기록됐다면 원자 교체를 하지 않아,
	// 직전의 정상 리포트가 잘린 파일로 덮여 유실되는 일을 막는다.
	const bool streamOk = (ferror(file) == 0);
	const bool closeOk = (fclose(file) == 0);
	if (!streamOk || !closeOk)
	{
		fprintf(stderr, "Profile: write to '%s' failed; keeping the previous report intact\n", tmpName);
		DeleteFileA(tmpName);
		ReleaseSRWLockExclusive(&g_printLock);
		return;
	}

	// 완성된 임시 파일을 최종 파일로 원자 교체
	if (!MoveFileExA(tmpName, szFileName, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
	{
		fprintf(stderr, "Profile: failed to replace '%s' (error %lu); report left in '%s'\n",
			szFileName, GetLastError(), tmpName);
	}

	ReleaseSRWLockExclusive(&g_printLock);
}

void ProfilePrint()
{
	ProfileDataOutText(DEFAULT_FILE_NAME);
}

void ProfileReset()
{
	AcquireSRWLockShared(&g_registryLock);
	ThreadBlock* head = g_blockHead;
	ReleaseSRWLockShared(&g_registryLock);

	for (ThreadBlock* block = head; block != nullptr; block = block->next)
	{
		AcquireSRWLockExclusive(&block->lock);
		block->used = 0;	// 진행 중(active) 측정도 함께 폐기된다
		ReleaseSRWLockExclusive(&block->lock);
	}

	AcquireSRWLockExclusive(&g_errorLock);
	g_errorCount = 0;
	g_errorTotal = 0;
	ReleaseSRWLockExclusive(&g_errorLock);
}
