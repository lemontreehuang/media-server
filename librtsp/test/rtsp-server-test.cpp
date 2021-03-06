#if defined(_DEBUG) || defined(DEBUG)
#include "cstringext.h"
#include "sys/sock.h"
#include "sys/thread.h"
#include "sys/system.h"
#include "sys/path.h"
#include "sys/sync.hpp"
#include "ctypedef.h"
#include "aio-socket.h"
#include "aio-timeout.h"
#include "ntp-time.h"
#include "rtp-profile.h"
#include "rtp-socket.h"
#include "rtsp-server.h"
#include "media/ps-file-source.h"
#include "media/h264-file-source.h"
#include "media/mp4-file-source.h"
#include "rtsp-server-aio.h"
#include "uri-parse.h"
#include "urlcodec.h"
#include "path.h"
#include <map>
#include <memory>
#include "cpm/shared_ptr.h"

#if defined(_HAVE_FFMPEG_)
#include "media/ffmpeg-file-source.h"
#endif

static const char* s_workdir = "e:";

static ThreadLocker s_locker;

struct rtsp_media_t
{
	std::shared_ptr<IMediaSource> media;
	socket_t socket[2];
	unsigned short port[2];
	int status; // setup-init, 1-play, 2-pause
};
typedef std::map<std::string, rtsp_media_t> TSessions;
static TSessions s_sessions;

struct TFileDescription
{
	int64_t duration;
	std::string sdpmedia;
};
static std::map<std::string, TFileDescription> s_describes;

static int rtsp_uri_parse(const char* uri, std::string& path)
{
	char path1[256];
	struct uri_t* r = uri_parse(uri, strlen(uri));
	if(!r)
		return -1;

	url_decode(r->path, strlen(r->path), path1, sizeof(path1));
	path = path1;
	uri_free(r);
	return 0;
}

static int rtsp_ondescribe(void* /*ptr*/, rtsp_server_t* rtsp, const char* uri)
{
	static const char* pattern =
		"v=0\n"
		"o=- %llu %llu IN IP4 %s\n"
		"s=%s\n"
		"c=IN IP4 0.0.0.0\n"
		"t=0 0\n"
		"a=range:npt=0-%.1f\n"
//		"a=range:npt=now-\n" // live
		"a=recvonly\n"
		"a=control:*\n"; // aggregate control

    std::string filename;
	std::map<std::string, TFileDescription>::const_iterator it;

	rtsp_uri_parse(uri, filename);
	assert(strstartswith(filename.c_str(), "/live/"));
	filename = path::join(s_workdir, filename.c_str()+6);

	{
		AutoThreadLocker locker(s_locker);
		it = s_describes.find(filename);
		if(it == s_describes.end())
		{
			// unlock
			TFileDescription describe;
			std::shared_ptr<IMediaSource> source;
//			source.reset(new PSFileSource(filename.c_str()));
//			source.reset(new H264FileSource(filename.c_str()));
#if defined(_HAVE_FFMPEG_)
			source.reset(new FFFileSource(filename.c_str()));
#else
			source.reset(new MP4FileSource(filename.c_str()));
#endif
			source->GetDuration(describe.duration);
			source->GetSDPMedia(describe.sdpmedia);

			// re-lock
			it = s_describes.insert(std::make_pair(filename, describe)).first;
		}
	}
    
	char buffer[1024];
	int offset = snprintf(buffer, sizeof(buffer), pattern, ntp64_now(), ntp64_now(), "0.0.0.0", uri, it->second.duration/1000.0);
	assert(offset > 0 && offset + 1 < sizeof(buffer));
	std::string sdp = buffer;
	sdp += it->second.sdpmedia;
    return rtsp_server_reply_describe(rtsp, 200, sdp.c_str());
}

static int rtsp_onsetup(void* /*ptr*/, rtsp_server_t* rtsp, const char* uri, const char* session, const struct rtsp_header_transport_t transports[], size_t num)
{
	std::string filename;
	char rtsp_transport[128];
	const struct rtsp_header_transport_t *transport = NULL;

	rtsp_uri_parse(uri, filename);
	assert(strstartswith(filename.c_str(), "/live/"));
	filename = path::join(s_workdir, filename.c_str() + 6);
	if ('\\' == *filename.rbegin() || '/' == *filename.rbegin())
		filename.erase(filename.end() - 1);
	const char* basename = path_basename(filename.c_str());
	if (NULL == strchr(basename, '.')) // filter track1
		filename.erase(basename - filename.c_str() - 1, std::string::npos);

	TSessions::iterator it;
	if(session)
	{
		AutoThreadLocker locker(s_locker);
		it = s_sessions.find(session);
		if(it == s_sessions.end())
		{
			// 454 Session Not Found
			return rtsp_server_reply_setup(rtsp, 454, NULL, NULL);
		}
		else
		{
			// don't support aggregate control
			if (0)
			{
				// 459 Aggregate Operation Not Allowed
				return rtsp_server_reply_setup(rtsp, 459, NULL, NULL);
			}
		}
	}
	else
	{
		rtsp_media_t item;
		memset(&item, 0, sizeof(item));
//		item.media.reset(new PSFileSource(filename.c_str()));
//		item.media.reset(new H264FileSource(filename.c_str()));
#if defined(_HAVE_FFMPEG_)
		item.media.reset(new FFFileSource(filename.c_str()));
#else
		item.media.reset(new MP4FileSource(filename.c_str()));
#endif

		char rtspsession[32];
		snprintf(rtspsession, sizeof(rtspsession), "%p", item.media.get());

		AutoThreadLocker locker(s_locker);
		it = s_sessions.insert(std::make_pair(rtspsession, item)).first;
	}

	assert(NULL == transport);
	for(size_t i = 0; i < num; i++)
	{
		if(RTSP_TRANSPORT_RTP_UDP == transports[i].transport)
		{
			// RTP/AVP/UDP
			transport = &transports[i];
			break;
		}
		else if(RTSP_TRANSPORT_RTP_TCP == transports[i].transport)
		{
			// RTP/AVP/TCP
			// 10.12 Embedded (Interleaved) Binary Data (p40)
			transport = &transports[i];
			break;
		}
	}
	if(!transport)
	{
		// 461 Unsupported Transport
		return rtsp_server_reply_setup(rtsp, 461, NULL, NULL);
	}

	if(transport->multicast)
	{
		// RFC 2326 1.6 Overall Operation p12
		// Multicast, client chooses address
		// Multicast, server chooses address
		assert(0);
	}
	else
	{
		// unicast
		assert(transport->rtp.u.client_port1 && transport->rtp.u.client_port2);

		rtsp_media_t &item = it->second;
		if(0 != rtp_socket_create(NULL, item.socket, item.port))
		{
			// log

			// 500 Internal Server Error
			return rtsp_server_reply_setup(rtsp, 500, NULL, NULL);
		}

		// RTP/AVP;unicast;client_port=4588-4589;server_port=6256-6257
		snprintf(rtsp_transport, sizeof(rtsp_transport), 
			"RTP/AVP;unicast;client_port=%hu-%hu;server_port=%hu-%hu", 
			transport->rtp.u.client_port1, transport->rtp.u.client_port2,
			item.port[0], item.port[1]);

		const char *ip = NULL;
		if(transport->destination[0])
		{
			ip = transport->destination;
			strcat(rtsp_transport, ";destination=");
			strcat(rtsp_transport, transport->destination);
		}
		else
		{
			ip = rtsp_server_get_client(rtsp, NULL);
		}

		unsigned short port[2] = { transport->rtp.u.client_port1, transport->rtp.u.client_port2 };
		item.media->SetRTPSocket(path_basename(uri), ip, item.socket, port);
	}

    return rtsp_server_reply_setup(rtsp, 200, it->first.c_str(), rtsp_transport);
}

static int rtsp_onplay(void* /*ptr*/, rtsp_server_t* rtsp, const char* uri, const char* session, const int64_t *npt, const double *scale)
{
	std::shared_ptr<IMediaSource> source;
	TSessions::iterator it;
	{
		AutoThreadLocker locker(s_locker);
		it = s_sessions.find(session ? session : "");
		if(it == s_sessions.end())
		{
			// 454 Session Not Found
			return rtsp_server_reply_play(rtsp, 454, NULL, NULL, NULL);
		}
		else
		{
			// uri with track
			if (0)
			{
				// 460 Only aggregate operation allowed
				return rtsp_server_reply_play(rtsp, 460, NULL, NULL, NULL);
			}
		}

		source = it->second.media;
	}
	if(npt && 0 != source->Seek(*npt))
	{
		// 457 Invalid Range
		return rtsp_server_reply_play(rtsp, 457, NULL, NULL, NULL);
	}

	if(scale && 0 != source->SetSpeed(*scale))
	{
		// set speed
		assert(scale > 0);

		// 406 Not Acceptable
		return rtsp_server_reply_play(rtsp, 406, NULL, NULL, NULL);
	}

	// RFC 2326 12.33 RTP-Info (p55)
	// 1. Indicates the RTP timestamp corresponding to the time value in the Range response header.
	// 2. A mapping from RTP timestamps to NTP timestamps (wall clock) is available via RTCP.
	char rtpinfo[512] = { 0 };
	source->GetRTPInfo(uri, rtpinfo, sizeof(rtpinfo));

	it->second.status = 1;
    return rtsp_server_reply_play(rtsp, 200, npt, NULL, rtpinfo);
}

static int rtsp_onpause(void* /*ptr*/, rtsp_server_t* rtsp, const char* /*uri*/, const char* session, const int64_t* /*npt*/)
{
	std::shared_ptr<IMediaSource> source;
	TSessions::iterator it;
	{
		AutoThreadLocker locker(s_locker);
		it = s_sessions.find(session ? session : "");
		if(it == s_sessions.end())
		{
			// 454 Session Not Found
			return rtsp_server_reply_pause(rtsp, 454);
		}
		else
		{
			// uri with track
			if (0)
			{
				// 460 Only aggregate operation allowed
				return rtsp_server_reply_pause(rtsp, 460);
			}
		}

		source = it->second.media;
		it->second.status = 2;
	}

	source->Pause();

	// 457 Invalid Range

    return rtsp_server_reply_pause(rtsp, 200);
}

static int rtsp_onteardown(void* /*ptr*/, rtsp_server_t* rtsp, const char* /*uri*/, const char* session)
{
	std::shared_ptr<IMediaSource> source;
	TSessions::iterator it;
	{
		AutoThreadLocker locker(s_locker);
		it = s_sessions.find(session ? session : "");
		if(it == s_sessions.end())
		{
			// 454 Session Not Found
			return rtsp_server_reply_teardown(rtsp, 454);
		}

		source = it->second.media;
		s_sessions.erase(it);
	}

	return rtsp_server_reply_teardown(rtsp, 200);
}

static int STDCALL rtsp_worker(void* /*param*/)
{
	while (aio_socket_process(200) >= 0 || errno == EINTR) // ignore epoll EINTR
	{
	}
	return 0;
}

extern "C" void rtsp_example()
{
	aio_socket_init(1);

	struct rtsp_handler_t handler;
	handler.ondescribe = rtsp_ondescribe;
    handler.onsetup = rtsp_onsetup;
    handler.onplay = rtsp_onplay;
    handler.onpause = rtsp_onpause;
    handler.onteardown = rtsp_onteardown;
    
	void* tcp = rtsp_server_listen(NULL, 554, &handler, NULL);
	void* udp = rtsp_transport_udp_create(NULL, 554, &handler, NULL);
	assert(tcp && udp);

	// create worker thread
	for (int i = 0; i < 1; i++)
	{
		pthread_t thread;
		thread_create(&thread, rtsp_worker, NULL);
		thread_detach(thread);
	}

	// test only
    while(1)
    {
		system_sleep(5);

		TSessions::iterator it;
		AutoThreadLocker locker(s_locker);
		for(it = s_sessions.begin(); it != s_sessions.end(); ++it)
		{
			rtsp_media_t &session = it->second;
			if(1 == session.status)
				session.media->Play();
		}

		aio_timeout_process();
    }

	aio_socket_clean();
	rtsp_server_unlisten(tcp);
	rtsp_transport_udp_destroy(udp);
}
#endif
