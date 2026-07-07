#include "Profile.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <cstdio>
#include <cstring>
#include <cfloat>

namespace
{
	constexpr int MAX_PROFILE_TAGS = 32;			// 스레드당 등록 가능한 태그 수
	constexpr int MAX_TAG_LEN = 40;					// 태그 이름 최대 길이(널 포함). 보고서 Name 컬럼 폭과 동일하게 유지한다
	constexpr int NAME_COL_W = MAX_TAG_LEN - 1;		// 저장 폭 == 표시 폭 → 잘림으로 인한 태그 혼동이 없다
	constexpr int MAX_ERRORS = 32;					// 보고서에 남길 오류 수
	constexpr int ERROR_MSG_LEN = 160;				// thread id + 메시지 + 태그가 잘리지 않을 크기
	constexpr long long CALLS_FOR_OUTLIER_CUT = 5;	// 상·하위 2개씩 제외하려면 최소 5회 필요

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
	SRWLOCK g_printLock = SRWLOCK_INIT;		// 모든 보고서 출력(자동/수동)을 직렬화해 동일 파일 동시 열기를 막는다
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

	ThreadBlock* CreateThreadBlock()
	{
		ThreadBlock* block = new ThreadBlock{};
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
		return tlsBlock;
	}

	// block->lock을 잡은 상태에서 호출할 것.
	// 저장 시 NAME_COL_W자로 잘리므로 비교도 같은 길이로 해야 긴 태그의 BEGIN/END가 짝을 찾는다
	ProfileSample* FindSlot(ThreadBlock* block, const char* tag)
	{
		for (int i = 0; i < block->used; i++)
		{
			if (strncmp(block->slots[i].tag, tag, NAME_COL_W) == 0)
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
	// 모든 출력을 직렬화한다: 자동 출력과 여러 스레드의 PRO_PRINT가 겹쳐도
	// 같은 파일을 동시에 열어 한쪽 리포트가 통째로 유실되는 일이 없다.
	AcquireSRWLockExclusive(&g_printLock);

	FILE* file = nullptr;
	if (fopen_s(&file, szFileName, "wt") != 0 || file == nullptr)
	{
		fprintf(stderr, "Profile: failed to open output file '%s'\n", szFileName);
		ReleaseSRWLockExclusive(&g_printLock);
		return;
	}

	// 헤더/데이터/구분선을 같은 폭 상수로 만들어 표가 절대 어긋나지 않게 한다
	char dash[9 + 3 + NAME_COL_W + 3 + 14 + 3 + 14 + 3 + 14 + 3 + 10 + 1];
	memset(dash, '-', sizeof(dash) - 1);
	dash[sizeof(dash) - 1] = '\0';

	fprintf(file, "%s\n", dash);
	fprintf(file, "%9s | %-*s | %14s | %14s | %14s | %10s\n",
		"ThreadId", NAME_COL_W, "Name", "Average(us)", "Min(us)", "Max(us)", "Call");
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

			fprintf(file, "%9lu | %-*.*s | %14.4f | %14.4f | %14.4f | %10lld\n",
				(unsigned long)threadId, NAME_COL_W, NAME_COL_W, s.tag, avg, mn, mx, s.call);
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
			fprintf(file, "(오류 %lld건 중 %lld건은 기록 한도 초과로 생략됨)\n",
				g_errorTotal, g_errorTotal - MAX_ERRORS);
	}
	ReleaseSRWLockShared(&g_errorLock);

	fclose(file);
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
