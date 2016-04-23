#include <time.h>
#include <arpa/inet.h>
#include <ncurses.h>
#include <sys/time.h>

#include "utlist.h"
#include "uthash.h"

#include "flow.h"
#include "timeywimey.h"
#include "intervals.h"

const int intervals[INTERVAL_COUNT] = {
	1E3,  5E3,   10E3,  20E3,
	50E3, 100E3, 200E3, 500E3
};

/* long, continuous sliding window tracking top flows */
static struct flow_hash *flow_ref_table = { NULL };

/* packet list enables removing expired packets from flow table */
static struct flow_pkt_list *pkt_list_ref_head = { NULL };

/* flows recorded as period-on-period intervals */
static struct flow_hash *incomplete_flow_tables[INTERVAL_COUNT] = { NULL };
static struct flow_hash *complete_flow_tables[INTERVAL_COUNT] = { NULL };

static struct timeval interval_end[INTERVAL_COUNT] = { 0 };
static struct timeval interval_start[INTERVAL_COUNT] = { 0 };

static void clear_table(int table_idx)
{
	struct flow_hash *table, *iter, *tmp;

	/* clear the complete table */
	table = complete_flow_tables[table_idx];
	HASH_ITER(ts_hh, table, iter, tmp) {
		HASH_DELETE(ts_hh, table, iter);
		free(iter);
        }
	assert(0 == HASH_CNT(ts_hh, table));
	complete_flow_tables[table_idx] = NULL;

	/* copy incomplete to complete */
	HASH_ITER(ts_hh, incomplete_flow_tables[table_idx], iter, tmp) {
		/* TODO: copy and insert */
		struct flow_hash *n = malloc(sizeof(struct flow_hash));
		memcpy(n, iter, sizeof(struct flow_hash));
		HASH_ADD(ts_hh, complete_flow_tables[table_idx], f.flow,
		         sizeof(struct flow), n);
	}
	assert(HASH_CNT(ts_hh, complete_flow_tables[table_idx])
	       == HASH_CNT(ts_hh, incomplete_flow_tables[table_idx]));

	/* clear the incomplete table */
	table = incomplete_flow_tables[table_idx];
	HASH_ITER(ts_hh, table, iter, tmp) {
		HASH_DELETE(ts_hh, table, iter);
		free(iter);
        }
	assert(0 == HASH_CNT(ts_hh, table));
	incomplete_flow_tables[table_idx] = NULL;
}

static void expire_old_interval_tables(struct timeval now)
{
	struct timeval tz = {0};

	for (int i = 0; i < INTERVAL_COUNT; i++) {
		struct timeval interval = {.tv_sec = 0,
			                   .tv_usec = intervals[i] };

		/* at start-up, end is still zero. initialise it. */
		if (0 == tv_cmp(tz, interval_end[i])) {
			interval_start[i] = now;
			interval_end[i] = tv_add(interval_start[i], interval);
		}

		/* interval elapsed? */
		if (0 < tv_cmp(now, interval_end[i])) {

			/* clear the hash table */
			clear_table(i);
			interval_start[i] = interval_end[i];
			interval_end[i] = tv_add(interval_end[i], interval);
		}
	}
}

static int bytes_cmp(struct flow_hash *f1, struct flow_hash *f2)
{
	return (f2->f.size - f1->f.size);
}

static int has_aged(struct flow_pkt *new_pkt, struct flow_pkt *old_pkt,
                    struct timeval max_age)
{
	struct timeval diff;

	diff = tv_absdiff(new_pkt->timestamp, old_pkt->timestamp);

	return (0 < tv_cmp(diff, max_age));
}

static void update_sliding_window_flow_ref(struct flow_pkt *pkt)
{
	struct flow_hash *fte;
	struct flow_pkt_list *ple, *tmp, *iter;
	struct timeval max_age = {.tv_sec = 5, .tv_usec = REF_INTERVAL };

	/* keep a list of packets, used for sliding window byte counts */
	ple = malloc(sizeof(struct flow_pkt_list));
	ple->pkt = *pkt;
	DL_APPEND(pkt_list_ref_head, ple);

	/* expire packets where time diff between current (ple) and prev (iter)
	 * is more than max_age */
	DL_FOREACH_SAFE(pkt_list_ref_head, iter, tmp)
	{
		if (has_aged(&(ple->pkt), &(iter->pkt), max_age)) {
			HASH_FIND(r_hh, flow_ref_table,
			          &(iter->pkt.flow_rec.flow),
			          sizeof(struct flow), fte);
			assert(fte);
			fte->f.size -= iter->pkt.flow_rec.size;
			if (0 == fte->f.size) {
				HASH_DELETE(r_hh, flow_ref_table, fte);
			}

			DL_DELETE(pkt_list_ref_head, iter);
			free(iter);
		} else {
			break;
		}
	}

	/* Update the flow accounting table */
	/* id already in the hash? */
	HASH_FIND(r_hh, flow_ref_table, &(pkt->flow_rec.flow),
	          sizeof(struct flow), fte);
	if (!fte) {
		fte = (struct flow_hash *)malloc(sizeof(struct flow_hash));
		memset(fte, 0, sizeof(struct flow_hash));
		memcpy(&(fte->f), &(pkt->flow_rec), sizeof(struct flow_record));
		HASH_ADD(r_hh, flow_ref_table, f.flow, sizeof(struct flow),
		         fte);
	} else {
		fte->f.size += pkt->flow_rec.size;
	}
}

static void add_flow_to_interval(struct flow_pkt *pkt, int time_series)
{
	struct flow_hash *fte;

	/* Update the flow accounting table */
	/* id already in the hash? */
	HASH_FIND(ts_hh, incomplete_flow_tables[time_series],
	          &(pkt->flow_rec.flow), sizeof(struct flow), fte);
	if (!fte) {
		fte = (struct flow_hash *)malloc(sizeof(struct flow_hash));
		memset(fte, 0, sizeof(struct flow_hash));
		memcpy(&(fte->f), &(pkt->flow_rec), sizeof(struct flow_record));
		HASH_ADD(ts_hh, incomplete_flow_tables[time_series], f.flow,
		         sizeof(struct flow), fte);
	} else {
		fte->f.size += pkt->flow_rec.size;
	}
}

void update_stats_tables(struct flow_pkt *pkt)
{
	update_sliding_window_flow_ref(pkt);

	for (int i = 0; i < INTERVAL_COUNT; i++) {
		add_flow_to_interval(pkt, i);
	}
	expire_old_interval_tables(pkt->timestamp);
}

static inline unsigned int rate_calc(int interval, int bytes)
{
	struct timeval t = { 0 };
	t.tv_usec = interval;
	double dt = t.tv_sec + t.tv_usec * 1E-9;
	return (unsigned int)(1.0 * bytes / dt / 1000);
}

static void fill_short_int_flows(struct flow_record st_flows[INTERVAL_COUNT],
                                 const struct flow_hash *ref_flow)
{
	struct flow_hash *fti; /* flow table iter (short-interval tables */
	struct flow_hash *te;  /* flow table entry */

	/* for each table in all time intervals.... */
	for (int i = INTERVAL_COUNT - 1; i >= 0; i--) {
		fti = complete_flow_tables[i];
		memcpy(&st_flows[i], &(ref_flow->f),
		       sizeof(struct flow_record));

		if (!fti) {
			/* table doesn't have anything in it yet */
			st_flows[i].size = 0;
			continue;
		}

		/* try to find the reference flow in the short flow table */
		HASH_FIND(ts_hh, fti, &(ref_flow->f.flow),
		          sizeof(struct flow), te);

		st_flows[i].size = te ? te->f.size : 0;

		/* convert to bytes per second */
		st_flows[i].size = rate_calc(intervals[i], st_flows[i].size);
	}
}

void get_top5(struct top_flows *t5)
{
	struct timeval now;
	struct flow_hash *rfti; /* reference flow table iter */

	/* sort the flow reference table */
	HASH_SRT(r_hh, flow_ref_table, bytes_cmp);

	gettimeofday(&now, NULL);
	expire_old_interval_tables(now);

	/* for each of the top 5 flow in the reference table,
	 * fill the counts from the short-interval flow tables */
	rfti = flow_ref_table;
	for (int i = 0; i < 5 && rfti; i++) {
		fill_short_int_flows(t5->flow[i], rfti);
		rfti = rfti->r_hh.next;
	}
}

int get_flow_count()
{
	return HASH_CNT(r_hh, flow_ref_table);
}
