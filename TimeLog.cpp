#include "TimeLog.h"
#include <sys/time.h>
#include <stdio.h>
#include <string>
#include <stdlib.h>
#include <stdarg.h>

#include <unistd.h>  
#include <sys/stat.h>  
#include <fcntl.h>  


static clock_t tTimer = 0;
static struct timeval stv;

const int MAX_FILE_SIZE = 1024 * 1024 * 2;
const char* path = "/logs/vr/vcyber.log";


CTimeLog::CTimeLog(void) :offset(0)
{


}

CTimeLog::~CTimeLog(void)
{

}

void CTimeLog::Init()
{
	offset = GetMsOffset();
	

	fd = open(path,(O_RDWR | O_CREAT |O_APPEND), 0644);  
 	new_fd = dup2(fd,STDOUT_FILENO); // 用我们新打开的文件描述符替换掉 标准输出 
}

void CTimeLog::CheckFileSize()
{

	 FILE *fp;
	 int len = 0;

	 fp = fopen(path, "r");
	 if( fp != NULL ) 
	 {
		fseek(fp, 0, SEEK_END);

	  	len = ftell(fp);
	  	fclose(fp);  
	 }
	
	if(len > MAX_FILE_SIZE)
	{
		remove(path);
		fd = open(path,(O_RDWR | O_CREAT |O_APPEND), 0644);  
 		new_fd = dup2(fd,STDOUT_FILENO); // 用我们新打开的文件描述符替换掉 标准输出 
	}

}

void CTimeLog::tprintf(const char * format, ...)
{
	char buf[1024] = {0};

	{
		struct timeval tv;
		struct tm* pt ;
		char sTime[32] = {0};
		time_t st;

		gettimeofday(&tv, NULL);
		tv.tv_usec /= 1000;
		st = tv.tv_sec;
		pt = localtime(&st);	

		strftime(sTime, 32, "[%Y-%m-%d %H:%M:%S.%%03d]", pt);
		sprintf(buf, sTime, tv.tv_usec);
	}

	{
		va_list args;
		va_start(args, format);
		vsprintf(buf + 25, format, args);
		va_end(args);
	}

	       
	if(fd < 0 || new_fd < 0)
	{
		return;   
	}

	     printf(buf);
	     fflush(stdout);
}

void CTimeLog::PrintTimeLog(const char* logStr)
{
	char tbuffer[64] = { 0 };
	char chtime[64] = { 0 };
	time_t t = time(NULL);
	strftime(tbuffer, 64, "%Y-%m-%d %H:%M:%S", localtime(&t));
	int SysMs = GetSysMs(offset);
	sprintf(chtime, "%s.%03d", tbuffer,SysMs);

	printf(logStr, chtime);
}

int CTimeLog::GetMsOffset()
{
	clock_t c =  clock();
	time_t tPre, tCur;
	tPre = time(NULL);
	for (tCur = time(NULL); tCur - tPre == 0; tPre = tCur, tCur = time(NULL), c = clock());
	return c;
}

int CTimeLog::GetSysMs(int offset)
{
	div_t tv = div((int)clock() - offset,CLOCKS_PER_SEC);
	return tv.rem;
}

//void CTimeLog::StartTime()
//{
//#ifdef _WIN32
//	tTimer = clock();
//#else
//	gettimeofday(&stv, NULL);
//#endif
//}
//
//void CTimeLog::EndTimeLog(const char * format)
//{
//#ifdef _WIN32
//		tprintf(format,clock()-tTimer);
//#else
//		struct timeval etv;
//		gettimeofday(&etv, NULL);
//		tprintf(format,(etv.tv_sec - stv.tv_sec)*1000 + (etv.tv_usec - stv.tv_usec)/1000);
//#endif	
//}

#ifdef _WIN32

void CTimeLog::EndTimeLog(const char * format,clock_t st)
{
	tprintf(format,clock()-st);
}
#else
void CTimeLog::EndTimeLog(const char * format,timeval stv)
{
	struct timeval etv;
	gettimeofday(&etv, NULL);
	tprintf(format,(etv.tv_sec - stv.tv_sec)*1000 + (etv.tv_usec - stv.tv_usec)/1000);
}
#endif	
