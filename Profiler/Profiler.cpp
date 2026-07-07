#include "Profile.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

int main()
{
	for (int i = 0; i < 10; i++)
	{
		PRO_BEGIN("hi");
		Sleep(25);
		PRO_END("hi");

		PRO_BEGIN("wawa");
		Sleep(1);
		PRO_END("wawa");
	}

	PRO_PRINT();
	return 0;
}
