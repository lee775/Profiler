#pragma once

// 프로파일링은 기본 활성화된다.
// 빌드 전체에서 측정 코드를 제거하려면 프로젝트 전처리기 정의에 PROFILE_DISABLE을 추가한다.
// (게이트 토큰을 별도 #define 하지 않는다 — 흔한 이름을 소비자 TU 전역에 누출하지 않기 위함)
#if !defined(PROFILE_DISABLE)
#define PRO_BEGIN(TagName)	ProfileBegin(TagName)
#define PRO_END(TagName)	ProfileEnd(TagName)
#define PRO_PRINT()			ProfilePrint()
#define PRO_RESET()			ProfileReset()
#else
#define PRO_BEGIN(TagName)
#define PRO_END(TagName)
#define PRO_PRINT()
#define PRO_RESET()
#endif

// 측정 구간 시작/종료.
// 태그 문자열은 내부 버퍼에 복사되며(최대 39자), 측정치는 호출한 스레드별로 집계된다.
void ProfileBegin(const char* tag);
void ProfileEnd(const char* tag);

// 결과 출력: ProfilePrint()는 profile.txt로, ProfileDataOutText()는 지정한 파일로 쓴다.
// 명시적으로 호출하지 않아도 프로세스가 정상 종료되면 profile.txt로 자동 출력된다.
void ProfilePrint();
void ProfileDataOutText(const char* szFileName);

// 모든 측정치와 오류 기록을 초기화한다. 진행 중인 측정(BEGIN~END 사이)이 없는 시점에 호출할 것.
void ProfileReset();
