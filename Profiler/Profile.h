#pragma once

#define PROFILE

#ifdef PROFILE
#define PRO_BEGIN(TagName)	ProfileBegin(TagName)
#define PRO_END(TagName)	ProfileEnd(TagName)
#define PRO_PRINT()	ProfilePrint()
#define PRO_RESET() ProfileReset()

#elif
#define PRO_BEGIN(TagName)
#define PRO_END(TagName)
#define PRO_PRINT()
#define PRO_RESET()
#endif

#include <Windows.h>
#include <iostream>

class Profile
{
public:
	const char* tag;
	LARGE_INTEGER startTime;
	//__int64 totalTime = 0;
	//__int64 min[2] = {MAXINT64, MAXINT64 };
	//__int64 max[2];
	double totalTime = 0;
	double min[2] = { 0xffffffff, 0xffffffff };
	double max[2];
	__int64 call = 0;
	bool con;

	Profile(const char* tag);
	~Profile();
	void ProfileDataOutText(FILE** file);
};
void ProfileBegin(const char* tag);
void ProfileEnd(const char* tag);
void ProfileDataOutText(char* szFileName);
void ProfileReset();

class ProfilePtr
{
public:
	Profile* profile = nullptr;
	~ProfilePtr();
};