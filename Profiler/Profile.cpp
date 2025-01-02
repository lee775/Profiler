#include "Profile.h"
#include <stdio.h>

//Profile* profile[20] = { nullptr };
ProfilePtr profilePtr[20];


Profile::Profile(const char* tag) :tag(tag)
{
	QueryPerformanceCounter(&this->startTime);
	this->max[0] = 0;
	this->max[1] = 0;

	this->min[0] = 0xffffffff;
	this->min[1] = 0xffffffff;
}

Profile::~Profile()
{
	FILE* file;
	fopen_s(&file, "profile.txt", "at");

	double avg = this->totalTime;

	for (int i = 0; i < 2; i++)
	{
		avg -= this->max[i];
		avg -= this->min[i];
	}

	avg = avg / (this->call - 4);
	fprintf(file, "%-20s | %12.4lf㎲ | %12.4lf㎲ | %12.4lf㎲ | %10lld\n", this->tag, avg, this->min[0], this->max[0], this->call);
	fclose(file);
}

void Profile::ProfileDataOutText(FILE** file)
{
	double avg = this->totalTime;
	for (int i = 0; i < 2; i++)
	{
		avg -= this->max[i];
		avg -= this->min[i];
	}
	avg = avg / (this->call - 4);
	fprintf(*file, "%-20s | %12.4lf㎲ | %12.4lf㎲ | %12.4lf㎲ | %10lld\n", this->tag, avg, this->min[0], this->max[0], this->call);
}

ProfilePtr::~ProfilePtr()
{
	delete profile;
}

void ProfileBegin(const char* tag)
{
	if (profilePtr[0].profile != nullptr)
	{
		for (int i = 0; i < 20; i++)
		{
			if (profilePtr[i].profile == nullptr)
			{
				profilePtr[i].profile = new Profile(tag);
				profilePtr[i].profile->call++;
				profilePtr[i].profile->con = true;
				QueryPerformanceCounter(&profilePtr[i].profile->startTime);
				break;
			}
			else if (strcmp(profilePtr[i].profile->tag, tag) == 0)
			{
				if (profilePtr[i].profile->con)
				{
					FILE* err;
					fopen_s(&err, "profile.txt", "at");
					char errText[] = "Consecutive calls to BEGIN";
					fprintf(err, "%s : %s\n", errText, tag);
					fclose(err);
					DebugBreak();
				}
				profilePtr[i].profile->call++;
				profilePtr[i].profile->con = true;
				QueryPerformanceCounter(&profilePtr[i].profile->startTime);
				break;
			}
		}
	}
	else
	{
		profilePtr[0].profile = new Profile(tag);
		profilePtr[0].profile->call++;
		profilePtr[0].profile->con = true;

		FILE* file;
		fopen_s(&file, "profile.txt", "wt");
		const char* header[] = {
			"--------------------------------------------------------------------------------------\n",
			"\n",
			"           Name |        Average |              Min |              Max |           Call |\n",
			"--------------------------------------------------------------------------------------\n"
		};

		for (int i = 0; i < sizeof(header) / sizeof(header[0]); i++) {
			fprintf(file, "%s", header[i]);
		}
		QueryPerformanceCounter(&profilePtr[0].profile->startTime);
		fclose(file);
	}
}

void ProfileEnd(const char* tag)
{
	LARGE_INTEGER freq;
	LARGE_INTEGER endTime;

	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&endTime);

	Profile* thisProfile = nullptr;
	for (int i = 0; i < 20; i++)
	{
		if (profilePtr[i].profile == nullptr)
		{
			FILE* err;
			fopen_s(&err, "profile.txt", "at");
			char errText[] = "END was called, but the object could not be found. \n";
			fprintf(err, "%s : %s\n", errText, tag);
			fclose(err);
			DebugBreak();
		}

		if (strcmp(profilePtr[i].profile->tag, tag) == 0)
		{
			thisProfile = profilePtr[i].profile;
			break;
		}
	}

	if (thisProfile == nullptr)
	{
		FILE* err;
		fopen_s(&err, "profile.txt", "at");
		char errText[] = "END was called, and the loop was also exited, but the object could not be found. \n";
		fprintf(err, "%s : %s\n", errText, tag);
		fclose(err);
		DebugBreak();
	}

	double totalTime = ((double)(endTime.QuadPart - thisProfile->startTime.QuadPart)) / freq.QuadPart;
	totalTime = totalTime * 1000000;
	thisProfile->totalTime += totalTime;

	for (int i = 0; i < 2; i++) {
		if (thisProfile->max[i] < totalTime) {
			thisProfile->max[i] = totalTime;

			// 정렬: 내림차순으로 유지
			if (thisProfile->max[0] < thisProfile->max[1]) {
				std::swap(thisProfile->max[0], thisProfile->max[1]);
			}
			break;
		}
	}

	for (int i = 0; i < 2; i++) {
		if (thisProfile->min[i] > totalTime) {
			thisProfile->min[i] = totalTime;

			// 정렬: 오름차순으로 유지
			if (thisProfile->min[0] > thisProfile->min[1]) {
				std::swap(thisProfile->min[0], thisProfile->min[1]);
			}
			break;
		}
	}

	thisProfile->con = false;
}

void ProfileDataOutText(char* szFileName)
{
	FILE* file;
	fopen_s(&file, szFileName, "at");

	int i = 0;
	while (profilePtr[i].profile != nullptr)
	{
		profilePtr[i].profile->ProfileDataOutText(&file);
		i++;
	}

	fclose(file);
}
