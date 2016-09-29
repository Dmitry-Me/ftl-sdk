#define __FTL_INTERNAL
#include "ftl.h"
#include "ftl_private.h"

#ifdef _WIN32
static DWORD WINAPI recv_thread(LPVOID data);
static DWORD WINAPI send_thread(LPVOID data);
#else
static void *recv_thread(void *data);
static void *send_thread(void *data);
#endif
static int _nack_init(ftl_media_component_common_t *media);
static int _nack_destroy(ftl_media_component_common_t *media);
static ftl_media_component_common_t *_media_lookup(ftl_stream_configuration_private_t *ftl, uint32_t ssrc);
static int _media_make_video_rtp_packet(ftl_stream_configuration_private_t *ftl, uint8_t *in, int in_len, uint8_t *out, int *out_len, int first_pkt);
static int _media_make_audio_rtp_packet(ftl_stream_configuration_private_t *ftl, uint8_t *in, int in_len, uint8_t *out, int *out_len);
static int _media_set_marker_bit(ftl_media_component_common_t *mc, uint8_t *in);
static int _media_send_packet(ftl_stream_configuration_private_t *ftl, uint32_t ssrc, uint16_t sn, int len);
static nack_slot_t* _media_get_empty_slot(ftl_stream_configuration_private_t *ftl, uint32_t ssrc, uint16_t sn);
static int _media_send_slot(ftl_stream_configuration_private_t *ftl, nack_slot_t *slot, BOOL is_retx);
static int _lock_slot(nack_slot_t *slot);
static int _unlock_slot(nack_slot_t *slot);

ftl_status_t media_init(ftl_stream_configuration_private_t *ftl) {

	ftl_media_config_t *media = &ftl->media;
	struct hostent *server = NULL;
	ftl_status_t status = FTL_SUCCESS;
	int i, idx;

	//Create a socket
	if ((media->media_socket = socket(AF_INET, SOCK_DGRAM, 0)) == INVALID_SOCKET)
	{
		FTL_LOG(FTL_LOG_ERROR, "Could not create socket : %s", ftl_get_socket_error());
	}
	FTL_LOG(FTL_LOG_INFO, "Socket created\n");

	if ((server = gethostbyname(ftl->ingest_ip)) == NULL) {
		FTL_LOG(FTL_LOG_ERROR, "No such host as %s\n", ftl->ingest_ip);
		return FTL_DNS_FAILURE;
	}

	//Prepare the sockaddr_in structure
	media->server_addr.sin_family = AF_INET;
	memcpy((char *)&media->server_addr.sin_addr.s_addr, (char *)server->h_addr, server->h_length);
	media->server_addr.sin_port = htons(media->assigned_port);

	media->max_mtu = MAX_MTU;

	ftl_media_component_common_t *media_comp[] = { &ftl->video.media_component, &ftl->audio.media_component };
	ftl_media_component_common_t *comp;

	for (idx = 0; idx < sizeof(media_comp) / sizeof(media_comp[0]); idx++) {

		comp = media_comp[idx];

		comp->nack_slots_initalized = FALSE;

		if ((status = _nack_init(comp)) != FTL_SUCCESS) {
			return status;
		}

		comp->timestamp = 0; //TODO: should start at a random value
		gettimeofday(&comp->stats, NULL);
		comp->producer = 0;
		comp->consumer = 0;
	}

	ftl->video.media_component.timestamp_step = (uint32_t)(90000.f / ftl->video.frame_rate);
	ftl->audio.media_component.timestamp_step = 48000 / 50; //TODO: dont assume the step size for audio

	media->recv_thread_running = TRUE;
#ifdef _WIN32
	if ((media->recv_thread_handle = CreateThread(NULL, 0, recv_thread, ftl, 0, &media->recv_thread_id)) == NULL) {
#else
	if ((pthread_create(&media->recv_thread, NULL, recv_thread, ftl)) != 0) {
#endif
		return FTL_MALLOC_FAILURE;
	}

	comp = &ftl->video.media_component;

#ifdef _WIN32
	if ((comp->send_frame_sem = CreateSemaphore(NULL, 0, 1000, NULL)) == NULL) {
#else
	comp->send_frame_sem
#endif
		return FTL_MALLOC_FAILURE;
	}

	media->send_thread_running = TRUE;
#ifdef _WIN32
	if ((media->send_thread_handle = CreateThread(NULL, 0, send_thread, ftl, 0, &media->send_thread_id)) == NULL) {
#else
	if ((pthread_create(&media->send_thread, NULL, send_thread, ftl)) != 0) {
#endif
		return FTL_MALLOC_FAILURE;
	}

	return status;
}

ftl_status_t media_destroy(ftl_stream_configuration_private_t *ftl) {
	ftl_media_config_t *media = &ftl->media;
	struct hostent *server = NULL;
	ftl_status_t status = FTL_SUCCESS;

	ftl_close_socket(media->media_socket);

	media->recv_thread_running = FALSE;
#ifdef _WIN32
	WaitForSingleObject(media->recv_thread_handle, INFINITE);
	CloseHandle(media->recv_thread_handle);
#else
	pthread_join(media->recv_thread, NULL);
#endif

	media->send_thread_running = FALSE;
#ifdef _WIN32
	ReleaseSemaphore(ftl->video.media_component.send_frame_sem, 1, NULL); 
	WaitForSingleObject(media->send_thread_handle, INFINITE);
	CloseHandle(media->send_thread_handle);
	CloseHandle(ftl->video.media_component.send_frame_sem);
#else
	pthread_join(media->send_thread, NULL);
#endif

	media->max_mtu = 0;

	ftl_media_component_common_t *video_comp = &ftl->video.media_component;

	_nack_destroy(video_comp);

	video_comp->timestamp = 0; //TODO: should start at a random value
	video_comp->timestamp_step = 0;

	ftl_media_component_common_t *audio_comp = &ftl->audio.media_component;

	_nack_destroy(audio_comp);

	audio_comp->timestamp = 0;
	audio_comp->timestamp_step = 0;
	
	return status;
}

static int _lock_slot(nack_slot_t *slot) {
#ifdef _WIN32
	WaitForSingleObject(slot->mutex, INFINITE);
#else
	pthread_mutex_lock(&slot->mutex);
#endif
	return 0;
}

static int _unlock_slot(nack_slot_t *slot) {
#ifdef _WIN32
	ReleaseMutex(slot->mutex);
#else
	pthread_mutex_unlock(&slot->mutex);
#endif

	return 0;
}

ftl_status_t media_send_audio(ftl_stream_configuration_private_t *ftl, uint8_t *data, int32_t len) {
	ftl_media_component_common_t *mc = &ftl->audio.media_component;
	uint8_t nalu_type = 0;

	int pkt_len;
	int payload_size;
	int consumed = 0;
	nack_slot_t *slot;
	int remaining = len;

	while (remaining > 0) {
		uint16_t sn = mc->seq_num;
		uint32_t ssrc = mc->ssrc;
		uint8_t *pkt_buf;
		
		slot = _media_get_empty_slot(ftl, ssrc, sn);
		pkt_buf = slot->packet;
		pkt_len = sizeof(slot->packet);

		_lock_slot(slot);

		payload_size = _media_make_audio_rtp_packet(ftl, data, remaining, pkt_buf, &pkt_len);

		remaining -= payload_size;
		consumed += payload_size;
		data += payload_size;

		slot->len = pkt_len;
		slot->sn = sn;
		gettimeofday(&slot->insert_time, NULL);

		_media_send_slot(ftl, slot, FALSE);

		_unlock_slot(slot);
	}

	return FTL_SUCCESS;
}

ftl_status_t media_send_video(ftl_stream_configuration_private_t *ftl, uint8_t *data, int32_t len, int end_of_frame){
	ftl_media_component_common_t *mc = &ftl->video.media_component;
	uint8_t nalu_type = 0;

	int pkt_len;
	int payload_size;
	int consumed = 0;
	nack_slot_t *slot;
	int remaining = len;
	int first_fu = 1;

	nalu_type = data[0] & 0x1F;

	while (remaining > 0) {
		uint16_t sn = mc->seq_num;
		uint32_t ssrc = mc->ssrc;
		uint8_t *pkt_buf;

		slot = _media_get_empty_slot(ftl, ssrc, sn);
		pkt_buf = slot->packet;
		pkt_len = sizeof(slot->packet);
		
		_lock_slot(slot);

		slot->first = 0;
		slot->last = 0;

		payload_size = _media_make_video_rtp_packet(ftl, data, remaining, pkt_buf, &pkt_len, first_fu);

		first_fu = 0;
		remaining -= payload_size;
		consumed += payload_size;
		data += payload_size;

		/*if all data has been consumed set marker bit*/
		if (remaining <= 0 && end_of_frame) { 
			_media_set_marker_bit(mc, pkt_buf);
			slot->last = 1;
		}

		slot->len = pkt_len;
		slot->sn = sn;
		gettimeofday(&slot->insert_time, NULL);

		_unlock_slot(slot);
	}

	struct timeval now, delta;
	gettimeofday(&now, NULL);
	timeval_subtract(&delta, &now, &mc->stats);
	float stats_interval = timeval_to_ms(&delta);

	if (stats_interval > 5000) {
		ftl_status_msg_t status;

		mc->stats = now;

		status.msg.video_stats.average_fps = 60;
		status.msg.video_stats.bytes_sent = 1234;
		status.msg.video_stats.frames_sent = 600;

		status.type = FTL_STATUS_VIDEO;

		enqueue_status_msg(ftl, &status);
	}

	if (end_of_frame) {
		ReleaseSemaphore(mc->send_frame_sem, 1, NULL);
	}

	return FTL_SUCCESS;
}

static int _nack_init(ftl_media_component_common_t *media) {

	for (int i = 0; i < NACK_RB_SIZE; i++) {
		if ((media->nack_slots[i] = (nack_slot_t *)malloc(sizeof(nack_slot_t))) == NULL) {
			FTL_LOG(FTL_LOG_ERROR, "Failed to allocate memory for nack buffer\n");
			return FTL_MALLOC_FAILURE;
		}

		nack_slot_t *slot = media->nack_slots[i];
		
#ifdef _WIN32
		if ((slot->mutex = CreateMutex(NULL, FALSE, NULL)) == NULL) {
#else
		if (pthread_mutex_init(&slot->mutex, NULL) != 0) {
#endif
			FTL_LOG(FTL_LOG_ERROR, "Failed to allocate memory for nack buffer\n");
			return FTL_MALLOC_FAILURE;
		}

		slot->len = 0;
		slot->sn = -1;
		//slot->insert_time = 0;
	}

	media->nack_slots_initalized = TRUE;
	media->seq_num = media->xmit_seq_num = 0; //TODO: should start at a random value

	return FTL_SUCCESS;
}

static int _nack_destroy(ftl_media_component_common_t *media) {

	for (int i = 0; i < NACK_RB_SIZE; i++) {
		if (media->nack_slots[i] != NULL) {
#ifdef _WIN32
			CloseHandle(media->nack_slots[i]->mutex);
#else
			pthread_mutex_destroy(&media->nack_slots[i]->mutex);
#endif
			free(media->nack_slots[i]);
			media->nack_slots[i] = NULL;
		}
	}

	return 0;
}

static ftl_media_component_common_t *_media_lookup(ftl_stream_configuration_private_t *ftl, uint32_t ssrc) {
	ftl_media_component_common_t *mc = NULL;

	/*check audio*/
	mc = &ftl->audio.media_component;
	if (mc->ssrc == ssrc) {
		return mc;
	}

	/*check video*/
	mc = &ftl->video.media_component;
	if (mc->ssrc == ssrc) {
		return mc;
	}

	return NULL;
}

static nack_slot_t* _media_get_empty_slot(ftl_stream_configuration_private_t *ftl, uint32_t ssrc, uint16_t sn) {
	ftl_media_component_common_t *mc;

	if ((mc = _media_lookup(ftl, ssrc)) == NULL) {
		FTL_LOG(FTL_LOG_ERROR, "Unable to find ssrc %d\n", ssrc);
		return NULL;
	}

	return mc->nack_slots[sn % NACK_RB_SIZE];
}

static int _media_send_slot(ftl_stream_configuration_private_t *ftl, nack_slot_t *slot, BOOL is_retx) {

	int tx_len;

	gettimeofday(&slot->xmit_time, NULL);
	
	if (!(slot->sn % 15000 == 0) || is_retx) {
		if ((tx_len = sendto(ftl->media.media_socket, slot->packet, slot->len, 0, (struct sockaddr*) &ftl->media.server_addr, sizeof(struct sockaddr_in))) == SOCKET_ERROR)
		{
			FTL_LOG(FTL_LOG_ERROR, "sendto() failed with error: %s", ftl_get_socket_error());
		}
	}
	else {
		tx_len = slot->len;
		FTL_LOG(FTL_LOG_INFO, "Dropping SN %d\n", slot->sn);
	}

	return tx_len;
}

//TODO: i dont like how mutexes are being handled here...rethink this
static int _nack_resend_packet(ftl_stream_configuration_private_t *ftl, uint32_t ssrc, uint16_t sn) {
	ftl_media_component_common_t *mc;
	int tx_len;

	if ((mc = _media_lookup(ftl, ssrc)) == NULL) {
		FTL_LOG(FTL_LOG_ERROR, "Unable to find ssrc %d\n", ssrc);
		return -1;
	}

	/*map sequence number to slot*/
	nack_slot_t *slot = mc->nack_slots[sn % NACK_RB_SIZE];
	_lock_slot(slot);

	if (slot->sn != sn) {
		FTL_LOG(FTL_LOG_WARN, "[%d] expected sn %d in slot but found %d...discarding retransmit request\n", ssrc, sn, slot->sn);
		_unlock_slot(slot);
		return 0;
	}

	int req_delay = 0;
	struct timeval delta, now;
	gettimeofday(&now, NULL);
	timeval_subtract(&delta, &now, &slot->xmit_time);
	req_delay = (int)timeval_to_ms(&delta);

	tx_len = _media_send_slot(ftl, slot, TRUE);
	FTL_LOG(FTL_LOG_INFO, "[%d] resent sn %d, request delay was %d ms\n", ssrc, sn, req_delay);

	_unlock_slot(slot);

	return tx_len;
}

static int _media_make_video_rtp_packet(ftl_stream_configuration_private_t *ftl, uint8_t *in, int in_len, uint8_t *out, int *out_len, int first_pkt) {
	uint8_t sbit, ebit;
	int frag_len;
	ftl_video_component_t *video = &ftl->video;
	ftl_media_component_common_t *mc = &video->media_component;

	sbit = first_pkt ? 1 : 0;
	ebit = (in_len + RTP_HEADER_BASE_LEN + RTP_FUA_HEADER_LEN) <= ftl->media.max_mtu;

	uint32_t rtp_header;

	rtp_header = htonl((2 << 30) | (mc->payload_type << 16) | mc->seq_num);
	*((uint32_t*)out)++ = rtp_header;
	rtp_header = htonl(mc->timestamp);
	*((uint32_t*)out)++ = rtp_header;
	rtp_header = htonl(mc->ssrc);
	*((uint32_t*)out)++ = rtp_header;

	mc->seq_num++;

	if (sbit && ebit) {
		sbit = ebit = 0;
		frag_len = in_len;
		*out_len = frag_len + RTP_HEADER_BASE_LEN;
		memcpy(out, in, frag_len);
	}
	else {

		if (sbit) {
			video->fua_nalu_type = in[0];
			in += 1;
			in_len--;
		}

		out[0] = (video->fua_nalu_type & 0x60) | 28;
		out[1] = (sbit << 7) | (ebit << 6) | (video->fua_nalu_type & 0x1F);

		out += 2;

		frag_len = ftl->media.max_mtu - RTP_HEADER_BASE_LEN - RTP_FUA_HEADER_LEN;

		if (frag_len > in_len) {
			frag_len = in_len;
		}

		memcpy(out, in, frag_len);

		*out_len = frag_len + RTP_HEADER_BASE_LEN + RTP_FUA_HEADER_LEN;
	}

	return frag_len + sbit;
}

static int _media_make_audio_rtp_packet(ftl_stream_configuration_private_t *ftl, uint8_t *in, int in_len, uint8_t *out, int *out_len) {
	int payload_len = in_len;

	uint32_t rtp_header;

	ftl_audio_component_t *audio = &ftl->audio;
	ftl_media_component_common_t *mc = &audio->media_component;

	rtp_header = htonl((2 << 30) | (1 << 23) | (mc->payload_type << 16) | mc->seq_num);
	*((uint32_t*)out)++ = rtp_header;
	rtp_header = htonl(mc->timestamp);
	*((uint32_t*)out)++ = rtp_header;
	rtp_header = htonl(mc->ssrc);
	*((uint32_t*)out)++ = rtp_header;

	mc->seq_num++;
	mc->timestamp += mc->timestamp_step;

	memcpy(out, in, payload_len);

	*out_len = payload_len + RTP_HEADER_BASE_LEN;

	return in_len;
}

static int _media_set_marker_bit(ftl_media_component_common_t *mc, uint8_t *in) {
	uint32_t rtp_header;

	rtp_header = ntohl(*((uint32_t*)in));
	rtp_header |= 1 << 23; /*set marker bit*/
	*((uint32_t*)in) = htonl(rtp_header);

	mc->timestamp += mc->timestamp_step;

	return 0;
}


/*handles rtcp packets from ingest including lost packet retransmission requests (nack)*/
#ifdef _WIN32
static DWORD WINAPI recv_thread(LPVOID data)
#else
static void *recv_thread(void *data)
#endif
{
	ftl_stream_configuration_private_t *ftl = (ftl_stream_configuration_private_t *)data;
	ftl_media_config_t *media = &ftl->media;
	int ret;
	unsigned char *buf;

#ifdef _WIN32
	if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL)) {
		FTL_LOG(FTL_LOG_WARN, "Failed to set recv_thread priority to THREAD_PRIORITY_TIME_CRITICAL\n");
	}
#endif

	if ((buf = (unsigned char*)malloc(MAX_PACKET_BUFFER)) == NULL) {
		FTL_LOG(FTL_LOG_ERROR, "Failed to allocate recv buffer\n");
		return -1;
	}

#if 0
	if (ret >= 0 && recv_size > 0) {
		if (!discard_recv_data(stream, (size_t)recv_size))
			return -1;
	}
#endif

	while (media->recv_thread_running) {

#ifdef _WIN32
		ret = recv(media->media_socket, buf, MAX_PACKET_BUFFER, 0);
#else
		ret = recv(stream->sb_socket, buf, MAX_PACKET_MTU, 0);
#endif
		if (ret <= 0) {
			continue;
		}

		int version, padding, feedbackType, ptype, length, ssrcSender, ssrcMedia;
		uint16_t snBase, blp, sn;
		int recv_len = ret;

		if (recv_len < 2) {
			FTL_LOG(FTL_LOG_WARN, "recv packet too small to parse, discarding\n");
			continue;
		}

		/*extract rtp header*/
		version = (buf[0] >> 6) & 0x3;
		padding = (buf[0] >> 5) & 0x1;
		feedbackType = buf[0] & 0x1F;
		ptype = buf[1];

		if (feedbackType == 1 && ptype == 205) {

			length = ntohs(*((uint16_t*)(buf + 2)));

			if (recv_len < ((length + 1) * 4)) {
				FTL_LOG(FTL_LOG_WARN, "reported len was %d but packet is only %d...discarding\n", recv_len, ((length + 1) * 4));
				continue;
			}

			ssrcSender = ntohl(*((uint32_t*)(buf + 4)));
			ssrcMedia = ntohl(*((uint32_t*)(buf + 8)));

			uint16_t *p = (uint16_t *)(buf + 12);

			for (int fci = 0; fci < (length - 2); fci++) {
				//request the first sequence number
				snBase = ntohs(*p++);
				_nack_resend_packet(ftl, ssrcMedia, snBase);
				blp = ntohs(*p++);
				if (blp) {
					for (int i = 0; i < 16; i++) {
						if ((blp & (1 << i)) != 0) {
							sn = snBase + i + 1;
							_nack_resend_packet(ftl, ssrcMedia, sn);
						}
					}
				}
			}
		}
	}

	free(buf);

	FTL_LOG(FTL_LOG_INFO, "Exited Recv Thread\n");

	return 0;
}



#ifdef _WIN32
static DWORD WINAPI send_thread(LPVOID data)
#else
static void *send_thread(void *data)
#endif
{
	ftl_stream_configuration_private_t *ftl = (ftl_stream_configuration_private_t *)data;
	ftl_media_config_t *media = &ftl->media;
	ftl_media_component_common_t *video = &ftl->video.media_component;
	int ret;
	nack_slot_t *slot;
	int sn;

	int first_packet = 1;
	int last_pkt_in_frame;
	int bytes_per_ms;

	int transmit_level;
	struct timeval start_tv, stop_tv, delta_tv;
	struct timeval profile_start, profile_stop, profile_delta;
	int pkt_xmit_delay_min = 1000, pkt_xmit_delay_max = 0, xmit_delay_delta;
	int xmit_delay_avg, xmit_delay_total = 0, xmit_delay_samples = 0;

#ifdef _WIN32
	if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL)) {
		FTL_LOG(FTL_LOG_WARN, "Failed to set recv_thread priority to THREAD_PRIORITY_TIME_CRITICAL\n");
	}
#endif

	bytes_per_ms = (((float)(ftl->video_kbps * 1000 / 8)) * 1.1) / 1000;

	transmit_level = MAX_XMIT_LEVEL_IN_MS/5 * bytes_per_ms;

	//bad design, this thread can spin
	while (1) {

#ifdef _WIN32
		WaitForSingleObject(video->send_frame_sem, INFINITE);
#else
		sem_pend
#endif

		if (!media->send_thread_running) {
			break;
		}

		while (transmit_level > 0 && video->xmit_seq_num != video->seq_num) {
			sn = video->xmit_seq_num;

			slot = video->nack_slots[video->xmit_seq_num % NACK_RB_SIZE];
			_lock_slot(slot);
			transmit_level -= _media_send_slot(ftl, slot, FALSE);
			timeval_subtract(&profile_delta, &slot->xmit_time, &slot->insert_time);
			last_pkt_in_frame = slot->last;
			_unlock_slot(slot);

			xmit_delay_delta = timeval_to_ms(&profile_delta);

			if (xmit_delay_delta > pkt_xmit_delay_max) {
				pkt_xmit_delay_max = xmit_delay_delta;
			}
			else if (xmit_delay_delta < pkt_xmit_delay_min) {
				pkt_xmit_delay_min = xmit_delay_delta;
			}

			xmit_delay_total += xmit_delay_delta;
			xmit_delay_samples++;

			video->xmit_seq_num++;

			if (last_pkt_in_frame) {
				break;
			}
		}

		if (xmit_delay_samples > 15000) {
			FTL_LOG(FTL_LOG_INFO, "Average transmit delay was %d ms (max: %d, min: %d)\n", xmit_delay_total/xmit_delay_samples, pkt_xmit_delay_max, pkt_xmit_delay_min);
			pkt_xmit_delay_min = 1000;
			pkt_xmit_delay_max = 0;
			xmit_delay_total = 0;
			xmit_delay_samples = 0;
		}
		
		if (transmit_level <= 0) {
			Sleep(1500/bytes_per_ms + 1);
		}

		gettimeofday(&stop_tv, NULL);
		if (!first_packet) {
			timeval_subtract(&delta_tv, &stop_tv, &start_tv);
			transmit_level += timeval_to_ms(&delta_tv) * bytes_per_ms;

			if (transmit_level > (MAX_XMIT_LEVEL_IN_MS * bytes_per_ms)) {
				transmit_level = MAX_XMIT_LEVEL_IN_MS * bytes_per_ms;
			}
		}
		else {
			first_packet = 0;
		}

		start_tv = stop_tv;
	}

	FTL_LOG(FTL_LOG_INFO, "Exited Send Thread\n");
	return 0;
}
