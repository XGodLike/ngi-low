#pragma once
#include <time.h>

class CTimeLog
{
public:
	CTimeLog(void);
	~CTimeLog(void);
public:
	 int save_fd ;
	 int fd; 
	 int new_fd;
public:
	//timeval vTime;
	int offset;
	void CheckFileSize();
	void tprintf(const char * format, ...);
	void PrintTimeLog(const char* logStr);
	int GetMsOffset();
	int GetSysMs(int offset);


#ifdef _WIN32
	void EndTimeLog(const char * format,clock_t st);
#else
	void EndTimeLog(const char * format,timeval stv);
#endif
};

