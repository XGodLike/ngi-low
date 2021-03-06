//SDK版本号
#define SDK_VERSION "1.1.10"
#include "DNS.h"

#ifndef _WIN32
#include <unistd.h>
#endif

#include <time.h>
#include <memory>
#include <queue>
#include <pthread.h>

#include <json/json.h>
#include <curl/curl.h>
#include "base64.hpp"
#include "tsilk.h"

#include "VcyberVoiceSDK.h"
#include "VcyberSpeex.h"

#include "Configs.h"
#include "SdkLib.h"
#include "TimeLog.h"
#include <sys/syscall.h>
#include <DNS.h>
using namespace std;

const int SILKSIZE = 700;

extern Configs g_configs;
extern CTimeLog* p_Timelog;
static CURL *curl = NULL;
static string session_id;
pthread_mutex_t seq_lock = PTHREAD_MUTEX_INITIALIZER;


//捕捉libcurl 域名解析超时信号
void  signal_function(int sig)
{
	if (g_configs.b_log) 
		p_Timelog->tprintf("[signal_function]tid = %ld;sig = %d\n",pthread_self(), sig);
	return ;
}
static void CurlInit(CURL **m_curl,string& data)
{
	*m_curl = curl_easy_init();

	curl_easy_setopt(*m_curl, CURLOPT_URL, g_configs.m_server_addr.c_str());
	//线程使用libcurl访问时，设置了超时时间，而libcurl库不会为这个超时信号做任何处理，
	//信号产生而没有信号句柄处理，可能导致程序退出。
	//用以下选项禁止访问超时的时候抛出超时信号。	   
	//curl_easy_setopt(*m_curl, CURLOPT_NOSIGNAL,1L);
	curl_easy_setopt(*m_curl, CURLOPT_POST, 1L);
	curl_easy_setopt(*m_curl, CURLOPT_WRITEFUNCTION, write_data_str);	
	curl_easy_setopt(*m_curl, CURLOPT_COOKIEFILE, "");	

	curl_slist* m_http_headers;
	string cookie = "Cookie: " + session_id;
	m_http_headers = curl_slist_append(NULL, "Content-Type:");
	m_http_headers = curl_slist_append(m_http_headers, "Accept:");
	m_http_headers = curl_slist_append(m_http_headers, "Expect:");
	m_http_headers = curl_slist_append(m_http_headers, cookie.c_str());
	m_http_headers = curl_slist_append(m_http_headers, "Connection: Keep-Alive");

	curl_easy_setopt(*m_curl, CURLOPT_HTTPHEADER, m_http_headers);
	curl_easy_setopt(*m_curl, CURLOPT_CONNECTTIMEOUT, 2L);
	curl_easy_setopt(*m_curl, CURLOPT_TIMEOUT, 2L);
	curl_easy_setopt(*m_curl, CURLOPT_POSTFIELDS, data.c_str());
}

static CURLcode PostData(ResultData* m_result ,string& data)
{	
	CURLcode r_code = CURLE_OK;
	CURL* m_curl = NULL;
	CurlInit(&m_curl,data);

	//通过回调函数获取服务器响应信息头、数据
	std::string rev_data;
	curl_easy_setopt(m_curl, CURLOPT_WRITEDATA, &rev_data);

	CURLcode res = curl_easy_perform(m_curl);
	if (res != CURLE_OK)
	{
		if (g_configs.b_log) {
			p_Timelog->tprintf("[PostData]curl_code=%d;try again\n",res);
		}	
		curl_easy_cleanup(m_curl);
		m_curl = NULL;
		CurlInit(&m_curl,data);
		
		//通过回调函数获取服务器响应信息头、数据
		curl_easy_setopt(m_curl, CURLOPT_WRITEDATA, &rev_data);

		res = curl_easy_perform(m_curl);
	}
	if (res == CURLE_OK)
	{
		//数据转化成json格式
		Json::Value jsonobj = get_data_str(rev_data);

		//语音输入法的结果
		if (!jsonobj["audio_text"].isNull())
		{
			std::string audio_text = jsonobj["audio_text"].asString();
			if (audio_text != "")
			{
				unsigned int len = audio_text.size() + 1;
				m_result->audio_text = (char*)realloc(m_result->audio_text, len + 1);
				strcpy(m_result->audio_text , audio_text.c_str());
			}
		}
		//语音识别结果
		if (!jsonobj["result"].isNull())
		{
			std::string out = jsonobj["result"].toStyledString();
			if (out != "")
			{
				unsigned int len = out.size() + 1;
				m_result->result  = (char*)realloc(m_result->result , len + 1);
				strcpy(m_result->result, out.c_str());
			}
			char sid[64] = { 0 };
			time_t lt = time(NULL);
			sprintf(sid, "%s%d", g_configs.m_vin.c_str(), lt);
			strcpy(m_result->uniqueSessionID, sid);
		}
		m_result->error_code = (eReturnCode)jsonobj["error_code"].asInt();
		if (m_result->error_code != 0 && g_configs.b_log)
		{
			p_Timelog->tprintf("[PostData]vd_code=%d\n",m_result->error_code);
		}
	}
	else
	{
		r_code = res;
		if (g_configs.b_log) {
			p_Timelog->tprintf("[PostData]curl_code=%d;return\n",res);
		}	
	}

	if (m_curl != NULL)
	{
		curl_easy_cleanup(m_curl);
		m_curl = NULL;
	}	

	return r_code;
}

static void *ThreadPostData(void* parameter)
{
	pthread_detach(pthread_self());
	dataBuf *m_thread = (dataBuf*)parameter;
	m_thread->b_start = true;

	struct timeval stv;
	gettimeofday(&stv, NULL);

	if (g_configs.b_log) {
		p_Timelog->tprintf("[ThreadPostData]ThreadPostData Start Time;b_start=%d\n",m_thread->b_start);			
	}
	
	while (m_thread->b_run)
	{
		if (!m_thread->q_buf.empty() && m_thread->c_code == CURLE_OK)
		{	
			if (g_configs.b_log) 
			{
				pthread_mutex_lock(&seq_lock);
				string data = m_thread->q_buf.front().buf;
				bool last = m_thread->q_buf.front().b_Last;
				m_thread->q_buf.pop();		
				pthread_mutex_unlock(&seq_lock);
				m_thread->c_code = PostData(&m_thread->m_result,data);
				p_Timelog->tprintf("[ThreadPostData]PostData;c_code=%d\n",m_thread->c_code);		
				if (last)
				{
					m_thread->b_last = true;
				}
			}	
			else
			{
				pthread_mutex_lock(&seq_lock);
				string data = m_thread->q_buf.front().buf;
				bool last = m_thread->q_buf.front().b_Last;
				m_thread->q_buf.pop();					
				pthread_mutex_unlock(&seq_lock);
				m_thread->c_code = PostData(&m_thread->m_result,data);
				if (last)
				{
					m_thread->b_last = true;
				}
			}
		}
		else
		{
			usleep(5000);
		}
	}

	m_thread->b_start = false;
	if (g_configs.b_log) 
	{
		p_Timelog->tprintf("[ThreadPostData]b_start=%d\n",m_thread->b_start);	
		p_Timelog->EndTimeLog("[ThreadPostData]ThreadPostData End Time[%d]\n",stv);
	}		
	return NULL;
}

static void *VDPostData(SESSION_HANDLE handle, const void * waveData, unsigned int dataLen, eAudioStatus dataStatus)
{
	struct timeval stv;
	gettimeofday(&stv, NULL);
	SessionParam *sp = (SessionParam*)handle;
	
	//silk压缩
	char* base64 = NULL;
	unsigned char* temp_data = NULL;
	if (sp->m_audio_quality <=10 && sp->m_audio_quality > 0)
	{
		int len = tsilk_in(sp->tsilk_enc, waveData, dataLen);
		if (len < SILKSIZE && dataStatus != AUDIO_STATUS_LAST)
		{
			return NULL;
		}
		//日志打印从这里开始，减少打印日志行数
		if (g_configs.b_log) {
			p_Timelog->tprintf("[CloudVDPostData]VDPostData Start Time;len=%d\n",dataLen);	
		}
		temp_data = new unsigned char[len];
		dataLen = tsilk_out(sp->tsilk_enc, temp_data, len);
		waveData = (char*)temp_data;
	}

	{
	//base64编码
		unsigned int out_len = 0;
		int base64_len = (dataLen * 4 / 3) + (dataLen % 3 ? 4 : 0) + 1;
		base64 = new char[base64_len];

		Base64::base64_encode((char*)waveData, dataLen, base64, &out_len);
		base64[out_len] = '\0';

		//传递一个作为HTTP “POST”操作的所有数据的字符串
		std::string data = get_data(sp, &g_configs, "PutAudio", base64, dataStatus);	
		sp->m_packet_id++;
		sp->m_packet_id++;
		qBuf m_buf;
		m_buf.buf = data;
		if (dataStatus == AUDIO_STATUS_LAST)
			m_buf.b_Last = true;
		else
			m_buf.b_Last = false;
		pthread_mutex_lock(&seq_lock);
		sp->m_data->q_buf.push(m_buf);		
		pthread_mutex_unlock(&seq_lock);
		//PostData(handle,data);
		delete base64;
		base64 = NULL;
		delete temp_data;
		temp_data = NULL;
	}
label:
	if (g_configs.b_log) 
	{
		p_Timelog->tprintf("[CloudVDPostData]queue.size = %d\n",sp->m_data->q_buf.size());
		p_Timelog->EndTimeLog("[CloudVDPostData]VDPostData End Time[%d]\n",stv);
	}	
	return NULL;
}

eReturnCode CloudVDInit(const char * configs) 
{	
	struct timeval stv;
	gettimeofday(&stv, NULL);

	curl_global_init(CURL_GLOBAL_ALL);
	curl = curl_easy_init();

	eReturnCode ret_code = CLOUDVD_SUCCESS;
	bool b_config = g_configs.Init(configs);
	if (b_config == false) {
		ret_code = CLOUDVD_ERR_INVALID_PARAM;
		goto label;
	}
	//日志控制
	if (g_configs.b_log)
	{
		p_Timelog = new CTimeLog();
		p_Timelog->Init();
	}

	if (g_configs.b_log) {
			p_Timelog->tprintf("[CloudVDInit]CloudVDInit Start Time\n");
			p_Timelog->tprintf("[CloudVDInit]sdk-version:%s\n",SDK_VERSION);
			p_Timelog->tprintf("[CloudVDInit]old_addr:%s\n",g_configs.m_server_addr.c_str());
	}
	if (g_configs.m_server_addr.size() == 0	|| g_configs.m_app_id.size() == 0
		|| g_configs.m_vin.size() == 0 || g_configs.m_mac.size() == 0) {
			ret_code = CLOUDVD_ERR_INVALID_PARAM;
			goto label;
	}

	if (g_configs.b_dns)
	{
		if (g_configs.b_log) 
			p_Timelog->tprintf("[CloudVDInit]DNS_pod Stime\n");

		g_configs.m_server_port = Get_Port((void*)g_configs.m_server_addr.c_str());//获取端口
		//g_configs.m_server_ip =  DNS_pod((void*)g_configs.m_server_addr.c_str());//智能DNS解析出来的IP
		g_configs.m_server_ip =  Parsing_IP(g_configs.m_server_addr.c_str());//智能DNS解析出来的IP

		if (g_configs.b_log)
			p_Timelog->tprintf("[CloudVDInit]DNS_pod Etime\n");
	
		if (g_configs.m_server_ip == "")
		{
			ret_code = CLOUDVD_ERR_NET_ACCESS_FAILED;
			goto label;
		}			

		g_configs.m_server_addr = "http://" + g_configs.m_server_ip + ":" + g_configs.m_server_port;
	}else
	{
		g_configs.m_server_port = Get_Port((void*)g_configs.m_server_addr.c_str());//获取端口
	}
	
	if (g_configs.b_log) 
		p_Timelog->tprintf("[CloudVDInit]server_addr:%s\n",g_configs.m_server_addr.c_str());


	curl_easy_setopt(curl, CURLOPT_URL, g_configs.m_server_addr.c_str());
	//线程使用libcurl访问时，设置了超时时间，而libcurl库不会为这个超时信号做任何处理，
	//信号产生而没有信号句柄处理，可能导致程序退出。
	//用以下选项禁止访问超时的时候抛出超时信号。
	//curl_easy_setopt(curl, CURLOPT_NOSIGNAL,1L);
	curl_easy_setopt(curl, CURLOPT_POST, 1L);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_data_str);
	curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, get_header_str);		
	curl_easy_setopt(curl, CURLOPT_COOKIEFILE, "");		
	//curl_easy_setopt(curl, CURLOPT_VERBOSE,1L);

label:
	if (g_configs.b_log) 
	{
		p_Timelog->tprintf("[CloudVDInit]funtion_code=%d\n",ret_code);
		p_Timelog->EndTimeLog("[CloudVDInit]CloudVDInit End Time[%d]\n",stv);
	}
	return ret_code;
}

eReturnCode CloudVDStartSession(const char * params, SESSION_HANDLE * handle) 
{	
	struct timeval stv;
	gettimeofday(&stv, NULL);

	if (g_configs.b_log) {
		p_Timelog->tprintf("[CloudVDStartSession]CloudVDStartSession Start Time\n");	
		p_Timelog->CheckFileSize();
	}


	struct sigaction actions;
	sigemptyset(&actions.sa_mask);  
	actions.sa_flags = 0;   
	actions.sa_handler = signal_function;  
	sigaction(SIGALRM,&actions,NULL);  

	//sigset_t mask;
	//sigfillset(&mask);/* 屏蔽所有信号*/
  	//sigemptyset(&mask);
  	//sigaddset(&mask, SIGALRM);
	//pthread_sigmask(SIG_SETMASK, &mask, NULL);

	CURLcode res = CURLE_OK;
	eReturnCode ret_code = CLOUDVD_SUCCESS;
	SessionParam *sp = new SessionParam(params);
	*handle = sp;

	sp->m_http_headers = curl_slist_append(NULL, "Content-Type:");
	sp->m_http_headers = curl_slist_append(sp->m_http_headers, "Accept:");
	sp->m_http_headers = curl_slist_append(sp->m_http_headers, "Expect:");
	sp->m_http_headers = curl_slist_append(sp->m_http_headers, "Connection: Keep-Alive");

	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, sp->m_http_headers);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, sp->m_time_out);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, sp->m_time_out);

	std::string data = get_data(sp, &g_configs, "SessionBegin", NULL, AUDIO_STATUS_CONTINUE);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data.c_str());

	std::string rev_data,rev_header;
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &rev_data);
	curl_easy_setopt(curl, CURLOPT_HEADERDATA , &rev_header);	
	
	if (g_configs.b_log){
		p_Timelog->tprintf("[CloudVDStartSession]curl_easy_perform Stime\n");
	}
	res = curl_easy_perform(curl);
	if (g_configs.b_log) {
		p_Timelog->tprintf("[CloudVDStartSession]curl_code=%d\n",res);
		p_Timelog->tprintf("[CloudVDStartSession]curl_easy_perform Etime\n");
	}

	if (CURLE_OK == res){
		//获取SessionID
		session_id = get_cookie_str(rev_header);
		//获取libcurl解析的ip地址
		char* ipstr = NULL;
		CURLcode info_code = curl_easy_getinfo(curl, CURLINFO_PRIMARY_IP, &ipstr);
		if (info_code == CURLE_OK)
		{
			g_configs.m_server_ip = ipstr;
			g_configs.m_server_addr = "http://" + g_configs.m_server_ip + ":" + g_configs.m_server_port;
		}
		if (g_configs.b_log)
		{
			p_Timelog->tprintf("[CloudVDStartSession]libcurl_dns_ip = %s\n",g_configs.m_server_ip.c_str());
			p_Timelog->tprintf("[CloudVDStartSession]%s\n",session_id.c_str());
		}

		Json::Value jsonobj = get_data_str(rev_data);
		sp->m_packet_id = jsonobj["packet_id"].asInt() + 1;
		sp->m_error_code = jsonobj["error_code"].asInt();
		if (sp->m_error_code != 0)
		{	
			if (g_configs.b_log)
			{
				p_Timelog->tprintf("[CloudVDStartSession]vd_code=%d\n",sp->m_error_code);
			}
			ret_code = CLOUDVD_ERR_NO_SUCH_GRAMMAR_SERVER;
			sp->m_state = CLOUDVD_ERR_NO_SUCH_GRAMMAR_SERVER;
			goto label;
		}
	} else {
		//ret_code =  (eReturnCode)res;
		ret_code = CLOUDVD_ERR_NET_ACCESS_FAILED;
		sp->m_state = CLOUDVD_ERR_NET_ACCESS_FAILED;
		goto label;
	}
	{
		//启动发送音频线程
		sp->m_data->b_start = false;//线程是否启动起来
		sp->m_data->b_run = true;//是否让线程继续运行
		sp->m_data->b_last = false;//最后一段音频是否发送完成
		sp->m_data->c_code = CURLE_OK ;//libcurl的返回码默认CURLE_OK
		pthread_t m_threadID;
		int ret = pthread_create(&m_threadID, NULL, ThreadPostData, (void*)sp->m_data);
		if(ret != 0)
		{
			p_Timelog->tprintf("[CloudVDStartSession]pthread start error:%d\n",ret);
			ret_code = CLOUDVD_ERR_SYSTEM_FAILED;
			goto label;
		}
		//确保线程启动起来
		while (!sp->m_data->b_start)
		{
			usleep(5000);
		}
	}

label:
	if (g_configs.b_log)
	{
		p_Timelog->tprintf("[CloudVDStartSession]function_code=%d\n",ret_code);
		p_Timelog->EndTimeLog("[CloudVDStartSession]CloudVDStartSession End Time[%d]\n",stv);
	}

	return ret_code;
}

eReturnCode CloudVDPostData(SESSION_HANDLE handle, const void * waveData, unsigned int dataLen, eAudioStatus dataStatus) 
{
	eReturnCode ret_code = CLOUDVD_SUCCESS;
	if (handle == NULL)
	{
		ret_code = CLOUDVD_ERR_NULL_POINTER;
		goto label;
	}

	VDPostData(handle,waveData,dataLen,dataStatus);
label: 
	return ret_code;
}

eReturnCode CloudVDGetResult(SESSION_HANDLE handle, ResultData** resultData) 
{	
	struct timeval stv;
	gettimeofday(&stv, NULL);

	if (g_configs.b_log){
		p_Timelog->tprintf("[CloudVDGetResult]CloudVDGetResult Start Time\n");
	}
	eReturnCode ret_code = CLOUDVD_SUCCESS;
	if (handle == NULL)
	{
		ret_code = CLOUDVD_ERR_NULL_POINTER;
		goto label;
	}
	{
		SessionParam *sp = (SessionParam*)handle;
		while (true) 
		{
			if (sp->m_data->b_last)
			{
				*resultData = (ResultData *)&sp->m_data->m_result;
				ret_code = sp->m_data->m_result.error_code;
				if (ret_code != 0)
				{
					ret_code = CLOUDVD_ERR_NO_SUCH_GRAMMAR_SERVER;
				}
				break;
			}
			else if(sp->m_data->c_code != CURLE_OK)
			{
				*resultData = (ResultData *)&sp->m_data->m_result;
				//ret_code = (eReturnCode)sp->m_data->c_code;
				ret_code = CLOUDVD_ERR_NET_ACCESS_FAILED;
				break;
			}
			else
			{
				usleep(5000);
			}	
		}
	}
label:	
	if (g_configs.b_log) 
	{
		p_Timelog->tprintf("[CloudVDGetResult]function_code=%d\n",ret_code);		
		p_Timelog->EndTimeLog("[CloudVDGetResult]CloudVDGetResult End Time[%d]\n",stv);
	}

	return (eReturnCode)ret_code;
}

eReturnCode CloudVDEndSession(SESSION_HANDLE * handle) 
{
	struct timeval stv;
	gettimeofday(&stv, NULL);

	if (g_configs.b_log){
		p_Timelog->tprintf("[CloudVDEndSession]CloudVDEndSession Start Time\n");
	}
	eReturnCode ret_code = eReturnCode(0);
	if (handle == NULL)
	{
		ret_code = CLOUDVD_ERR_NULL_POINTER;
		goto label;
	}

	{
		
		SessionParam *sp = (SessionParam*)*handle;
		sp->m_data->b_run = false;//终止线程
		//确保线程终止
		while (sp->m_data->b_start)
		{
			usleep(5000);
		}

		if(sp->m_state != CLOUDVD_ERR_NET_ACCESS_FAILED)
		{
			std::string data = get_data(sp, &g_configs, "SessionEnd", "", (eAudioStatus)NULL);
			curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data.c_str());

			std::string rev_data,rev_header;
			curl_easy_setopt(curl, CURLOPT_WRITEDATA, &rev_data);
			curl_easy_setopt(curl, CURLOPT_HEADERDATA, &rev_header); 
	
			if (g_configs.b_log) {
				p_Timelog->tprintf("[CloudVDEndSession]curl_easy_perform Stime\n");
			}
			CURLcode res = curl_easy_perform(curl);
			if (g_configs.b_log) {
				p_Timelog->tprintf("[CloudVDEndSession]curl_code=%d\n",res);
				p_Timelog->tprintf("[CloudVDEndSession]curl_easy_perform Etime\n");
			}

			if (CURLE_OK == res){
				ret_code = CLOUDVD_SUCCESS;
			} else {
				ret_code =  CLOUDVD_ERR_NET_ACCESS_FAILED;
			}
		}
		else
		{
			ret_code =  CLOUDVD_ERR_NET_ACCESS_FAILED;
		}
		
		
		if (sp != NULL)
		{
			delete sp;
			sp = NULL;
		}
	}
label:
	if (g_configs.b_log) 
	{
		p_Timelog->tprintf("[CloudVDEndSession]function_code=%d\n",ret_code);
		p_Timelog->EndTimeLog("[CloudVDEndSession]CloudVDEndSession End Time[%d]\n",stv);
	}
	return ret_code;
}

eReturnCode CloudVDFini() {
	if (g_configs.b_log) {
		p_Timelog->tprintf("[CloudVDFini]CloudVDFini Start Time\n");
	}
	curl_easy_cleanup(curl);
	curl = NULL;
	curl_global_cleanup();
	if (g_configs.b_log)
	{
		p_Timelog->tprintf("[CloudVDFini]CloudVDFini End Time\n");
		delete p_Timelog;
		p_Timelog = NULL;
	}
	return CLOUDVD_SUCCESS;
}
