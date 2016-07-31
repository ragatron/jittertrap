#include <net/ethernet.h>
#include <arpa/inet.h>

#include <pthread.h>

#include <jansson.h>

#include "jt_message_types.h"
#include "jt_messages.h"

#include "mq_msg_tt.h"

#include "flow.h"
#include "intervals_user.h"
#include "intervals.h"

#include "tt_thread.h"

struct tt_thread_info ti = { 0 };

struct intervals_thread_info {
	pthread_t thread_id;
	pthread_attr_t thread_attr;
};

struct intervals_thread_info iti = { 0 };

static char const *const protos[IPPROTO_MAX] = {[IPPROTO_TCP] = "TCP",
                                                [IPPROTO_UDP] = "UDP",
                                                [IPPROTO_ICMP] = "ICMP",
                                                [IPPROTO_ICMPV6] = "ICMP6",
                                                [IPPROTO_IP] = "IP",
                                                [IPPROTO_IGMP] = "IGMP" };

int tt_thread_restart(char * iface)
{
	int err;
	void *res;

	if (ti.thread_id) {
		pthread_cancel(ti.thread_id);
		pthread_join(ti.thread_id, &res);
		free(ti.t5);
		free(ti.dev);
	}

	ti.dev = malloc(MAX_IFACE_LEN);
	snprintf(ti.dev, MAX_IFACE_LEN, "%s", iface);

	/* start & run thread for capture and interval processing */
	tt_intervals_init(&ti);

	err = pthread_attr_init(&ti.attr);
	assert(!err);

	err = pthread_create(&ti.thread_id, &ti.attr, tt_intervals_run, &ti);
	assert(!err);
        pthread_setname_np(ti.thread_id, "jt-toptalk");

	tt_update_ref_window_size(tt_intervals[0]);
	tt_update_ref_window_size(tt_intervals[INTERVAL_COUNT - 1]);

	return 0;
}

/* Convert from a struct tt_top_flows to a struct mq_tt_msg */
static int
m2m(struct tt_top_flows *ttf, struct mq_tt_msg *msg, int interval)
{
	struct timespec t = {0};
	struct jt_msg_toptalk *m = &msg->m;

	m->timestamp = t;
	m->interval_ns = tt_intervals[interval].tv_sec * 1E9
		+ tt_intervals[interval].tv_usec * 1E3;

	m->tflows = ttf->flow_count;
	m->tbytes = ttf->total_bytes;
	m->tpackets = ttf->total_packets;

	for (int f = 0; f < MAX_FLOWS; f++) {
		m->flows[f].bytes = ttf->flow[f][interval].bytes;
		m->flows[f].packets = ttf->flow[f][interval].packets;
		m->flows[f].sport = ttf->flow[f][interval].flow.sport;
		m->flows[f].dport = ttf->flow[f][interval].flow.dport;
		snprintf(m->flows[f].proto, PROTO_LEN, "%s",
				protos[ttf->flow[f][interval].flow.proto]);
		snprintf(m->flows[f].src, ADDR_LEN, "%s",
				inet_ntoa(ttf->flow[f][interval].flow.src_ip));
		snprintf(m->flows[f].dst, ADDR_LEN, "%s",
				inet_ntoa(ttf->flow[f][interval].flow.dst_ip));
	}
	return 0;
}

inline static int message_producer(struct mq_tt_msg *m, void *data)
{

	memcpy(m, (struct mq_tt_msg *)data, sizeof(struct mq_tt_msg));
	return 0;
}

int queue_tt_msg(int interval)
{
	struct mq_tt_msg msg;
	struct tt_top_flows *t5 = ti.t5;
	int cb_err;

	pthread_mutex_lock(&ti.t5_mutex);
	{
		m2m(t5, &msg, interval);
		mq_tt_produce(message_producer, &msg, &cb_err);
	}
	pthread_mutex_unlock(&ti.t5_mutex);
	return 0;
}


/* TODO: calculate the GCD of tt_intervals
 * updates output var intervals
 * returns GCD nanoseconds*/
static uint32_t calc_intervals(uint32_t intervals[INTERVAL_COUNT])
{
	uint64_t t0_us = tt_intervals[0].tv_sec * 1E6 + tt_intervals[0].tv_usec;

	for (int i = INTERVAL_COUNT - 1; i >= 0; i--) {
		uint64_t t_us = tt_intervals[i].tv_sec * 1E6
		                + tt_intervals[i].tv_usec;
		intervals[i] = t_us / t0_us;

		/* FIXME: for now, t0_us is the GCD of tt_intervals */
		assert(0 == t_us % t0_us);
	}
	return 1E3 * tt_intervals[0].tv_usec + 1E9 * tt_intervals[0].tv_sec;
}


static void *intervals_run(void *data)
{
	(void)data; /* unused */
	struct timespec deadline;

	uint32_t tick = 0;
	/* integer multiple of gcd in interval */
	uint32_t imuls[INTERVAL_COUNT];
	uint32_t sleep_time_ns = calc_intervals(imuls);

	clock_gettime(CLOCK_MONOTONIC, &deadline);

	for (;;) {

		for (int i = 0; i < INTERVAL_COUNT; i++) {
			assert(imuls[i]);
			if (0 == (tick % imuls[i])) {
				queue_tt_msg(i);
			}
		}

		/* increment / wrap tick */
		tick = (imuls[INTERVAL_COUNT-1] == tick) ? 0 : tick + 1;

		deadline.tv_nsec += sleep_time_ns;

		/* Second boundary */
		if (deadline.tv_nsec >= 1E9) {
			deadline.tv_nsec -= 1E9;
			deadline.tv_sec++;
		}

		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline,
		                NULL);
	}
	return NULL;
}

int intervals_thread_init()
{
	int err;
	void *res;

	if (iti.thread_id) {
		pthread_cancel(iti.thread_id);
		pthread_join(iti.thread_id, &res);
	}

	err = pthread_attr_init(&iti.thread_attr);
	assert(!err);

	err = pthread_create(&iti.thread_id, &iti.thread_attr, intervals_run,
	                     NULL);
	assert(!err);
	pthread_setname_np(iti.thread_id, "jt-intervals");

	return 0;
}
