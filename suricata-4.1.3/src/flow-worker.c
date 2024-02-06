/* Copyright (C) 2016 Open Information Security Foundation
 *
 * You can copy, redistribute or modify this Program under the terms of
 * the GNU General Public License version 2 as published by the Free
 * Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

/**
 * \file
 *
 * \author Victor Julien <victor@inliniac.net>
 *
 * Flow Workers are single thread modules taking care of (almost)
 * everything related to packets with flows:
 *
 * - Lookup/creation
 * - Stream tracking, reassembly
 * - Applayer update
 * - Detection
 *
 * This all while holding the flow lock.
 */

#include "suricata-common.h"
#include "suricata.h"

#include "decode.h"
#include "stream-tcp.h"
#include "app-layer.h"
#include "detect-engine.h"
#include "output.h"
#include "app-layer-parser.h"

#include "util-validate.h"

#include "flow-util.h"

typedef DetectEngineThreadCtx *DetectEngineThreadCtxPtr;

typedef struct FlowWorkerThreadData_ {
    DecodeThreadVars *dtv;

    union {
        StreamTcpThread *stream_thread;
        void *stream_thread_ptr;
    };

    SC_ATOMIC_DECLARE(DetectEngineThreadCtxPtr, detect_thread);

    void *output_thread; /* Output thread data. */

    PacketQueue pq;

} FlowWorkerThreadData;

/** \brief handle flow for packet
 *
 *  Handle flow creation/lookup
 */
static inline TmEcode FlowUpdate(Packet *p)
{
    FlowHandlePacketUpdate(p->flow, p);

    int state = SC_ATOMIC_GET(p->flow->flow_state);
    switch (state) {
        case FLOW_STATE_CAPTURE_BYPASSED:
        case FLOW_STATE_LOCAL_BYPASSED:
            return TM_ECODE_DONE;
        default:
            return TM_ECODE_OK;
    }
}

static TmEcode FlowWorkerThreadDeinit(ThreadVars *tv, void *data);

static TmEcode FlowWorkerThreadInit(ThreadVars *tv, const void *initdata, void **data)
{
    FlowWorkerThreadData *fw = SCCalloc(1, sizeof(*fw));
    if (fw == NULL)
        return TM_ECODE_FAILED;

    SC_ATOMIC_INIT(fw->detect_thread);
    SC_ATOMIC_SET(fw->detect_thread, NULL);

    fw->dtv = DecodeThreadVarsAlloc(tv);
    if (fw->dtv == NULL) {
        FlowWorkerThreadDeinit(tv, fw);
        return TM_ECODE_FAILED;
    }

    /* setup TCP */
    if (StreamTcpThreadInit(tv, NULL, &fw->stream_thread_ptr) != TM_ECODE_OK) {
        FlowWorkerThreadDeinit(tv, fw);
        return TM_ECODE_FAILED;
    }

    if (DetectEngineEnabled()) {
        /* setup DETECT */
        void *detect_thread = NULL;
        if (DetectEngineThreadCtxInit(tv, NULL, &detect_thread) != TM_ECODE_OK) {
            FlowWorkerThreadDeinit(tv, fw);
            return TM_ECODE_FAILED;
        }
        SC_ATOMIC_SET(fw->detect_thread, detect_thread);
    }

    /* Setup outputs for this thread. */
    if (OutputLoggerThreadInit(tv, initdata, &fw->output_thread) != TM_ECODE_OK) {
        FlowWorkerThreadDeinit(tv, fw);
        return TM_ECODE_FAILED;
    }

    DecodeRegisterPerfCounters(fw->dtv, tv);
    AppLayerRegisterThreadCounters(tv);

    /* setup pq for stream end pkts */
    memset(&fw->pq, 0, sizeof(PacketQueue));
    SCMutexInit(&fw->pq.mutex_q, NULL);

    *data = fw;
    return TM_ECODE_OK;
}

static TmEcode FlowWorkerThreadDeinit(ThreadVars *tv, void *data)
{
    FlowWorkerThreadData *fw = data;

    DecodeThreadVarsFree(tv, fw->dtv);

    /* free TCP */
    StreamTcpThreadDeinit(tv, (void *)fw->stream_thread);

    /* free DETECT */
    void *detect_thread = SC_ATOMIC_GET(fw->detect_thread);
    if (detect_thread != NULL) {
        DetectEngineThreadCtxDeinit(tv, detect_thread);
        SC_ATOMIC_SET(fw->detect_thread, NULL);
    }

    /* Free output. */
    OutputLoggerThreadDeinit(tv, fw->output_thread);

    /* free pq */
    BUG_ON(fw->pq.len);
    SCMutexDestroy(&fw->pq.mutex_q);

    SC_ATOMIC_DESTROY(fw->detect_thread);
    SCFree(fw);
    return TM_ECODE_OK;
}

TmEcode Detect(ThreadVars *tv, Packet *p, void *data, PacketQueue *pq, PacketQueue *postpq);
TmEcode StreamTcp (ThreadVars *, Packet *, void *, PacketQueue *, PacketQueue *);
// data是flowworker模块初始化的数据
static TmEcode FlowWorker(ThreadVars *tv, Packet *p, void *data, PacketQueue *preq, PacketQueue *unused)
{
    FlowWorkerThreadData *fw = data;
	// 获取检测引擎的数据，这个数据每个woker线程一份
    void *detect_thread = SC_ATOMIC_GET(fw->detect_thread);

    SCLogDebug("packet %"PRIu64, p->pcap_cnt);

    /* update time */
    if (!(PKT_IS_PSEUDOPKT(p))) {
        TimeSetByThread(tv->id, &p->ts);
    }

    /* handle Flow */
    if (p->flags & PKT_WANTS_FLOW) {
		// 用于计算性能分析结论。需要开启profiling
        FLOWWORKER_PROFILING_START(p, PROFILE_FLOWWORKER_FLOW);
		// 流表匹配，找到对应数据包的那条流
        FlowHandlePacket(tv, fw->dtv, p);
        if (likely(p->flow != NULL)) {
            DEBUG_ASSERT_FLOW_LOCKED(p->flow);
			// 根据包/流特征更新标志位，处理优先级。填充关于流向信息，自增双边包数。
            if (FlowUpdate(p) == TM_ECODE_DONE) {
                FLOWLOCK_UNLOCK(p->flow);
                return TM_ECODE_OK;
            }
        }
        /* Flow is now LOCKED */
		// 用于计算性能分析结论，需要开启profiling
        FLOWWORKER_PROFILING_END(p, PROFILE_FLOWWORKER_FLOW);

    /* if PKT_WANTS_FLOW is not set, but PKT_HAS_FLOW is, then this is a
     * pseudo packet created by the flow manager. */
    } else if (p->flags & PKT_HAS_FLOW) {
        FLOWLOCK_WRLOCK(p->flow);
    }

    SCLogDebug("packet %"PRIu64" has flow? %s", p->pcap_cnt, p->flow ? "yes" : "no");

    /* handle TCP and app layer */
	// 处理tcp数据
    if (p->flow && PKT_IS_TCP(p)) {
        SCLogDebug("packet %"PRIu64" is TCP. Direction %s", p->pcap_cnt, PKT_IS_TOSERVER(p) ? "TOSERVER" : "TOCLIENT");
        DEBUG_ASSERT_FLOW_LOCKED(p->flow);

        /* if detect is disabled, we need to apply file flags to the flow
         * here on the first packet. */
        if (detect_thread == NULL &&
                ((PKT_IS_TOSERVER(p) && (p->flowflags & FLOW_PKT_TOSERVER_FIRST)) ||
                 (PKT_IS_TOCLIENT(p) && (p->flowflags & FLOW_PKT_TOCLIENT_FIRST))))
        {
        	// 检测引擎未开启的状态下，禁用一些关于文件留存的功能。
            DisableDetectFlowFileFlags(p->flow);
        }

        FLOWWORKER_PROFILING_START(p, PROFILE_FLOWWORKER_STREAM);
		// fw->stream_thread 这个处理线程对tcp流的统计数据 
		// fw->pq 这个处理线程对处理的数据包队列
        StreamTcp(tv, p, fw->stream_thread, &fw->pq, NULL);
        FLOWWORKER_PROFILING_END(p, PROFILE_FLOWWORKER_STREAM);

        if (FlowChangeProto(p->flow)) {
            StreamTcpDetectLogFlush(tv, fw->stream_thread, p->flow, p, &fw->pq);
        }

        /* Packets here can safely access p->flow as it's locked */
        SCLogDebug("packet %"PRIu64": extra packets %u", p->pcap_cnt, fw->pq.len);
        Packet *x;
        while ((x = PacketDequeue(&fw->pq))) {
            SCLogDebug("packet %"PRIu64" extra packet %p", p->pcap_cnt, x);

            // TODO do we need to call StreamTcp on these pseudo packets or not?
            //StreamTcp(tv, x, fw->stream_thread, &fw->pq, NULL);
            if (detect_thread != NULL) {
                FLOWWORKER_PROFILING_START(x, PROFILE_FLOWWORKER_DETECT);
                Detect(tv, x, detect_thread, NULL, NULL);
                FLOWWORKER_PROFILING_END(x, PROFILE_FLOWWORKER_DETECT);
            }

            //  Outputs
            OutputLoggerLog(tv, x, fw->output_thread);

            /* put these packets in the preq queue so that they are
             * by the other thread modules before packet 'p'. */
            PacketEnqueue(preq, x);
        }

    /* handle the app layer part of the UDP packet payload */
    } else if (p->flow && p->proto == IPPROTO_UDP) {
        FLOWWORKER_PROFILING_START(p, PROFILE_FLOWWORKER_APPLAYERUDP);
        AppLayerHandleUdp(tv, fw->stream_thread->ra_ctx->app_tctx, p, p->flow);
        FLOWWORKER_PROFILING_END(p, PROFILE_FLOWWORKER_APPLAYERUDP);
    }

    PacketUpdateEngineEventCounters(tv, fw->dtv, p);

    /* handle Detect */
    DEBUG_ASSERT_FLOW_LOCKED(p->flow);
    SCLogDebug("packet %"PRIu64" calling Detect", p->pcap_cnt);

    if (detect_thread != NULL) {
        FLOWWORKER_PROFILING_START(p, PROFILE_FLOWWORKER_DETECT);
        Detect(tv, p, detect_thread, NULL, NULL);
        FLOWWORKER_PROFILING_END(p, PROFILE_FLOWWORKER_DETECT);
    }

    // Outputs.
    OutputLoggerLog(tv, p, fw->output_thread);

    /*  Release tcp segments. Done here after alerting can use them. */
    if (p->flow != NULL && p->proto == IPPROTO_TCP) {
        FLOWWORKER_PROFILING_START(p, PROFILE_FLOWWORKER_TCPPRUNE);
        StreamTcpPruneSession(p->flow, p->flowflags & FLOW_PKT_TOSERVER ?
                STREAM_TOSERVER : STREAM_TOCLIENT);
        FLOWWORKER_PROFILING_END(p, PROFILE_FLOWWORKER_TCPPRUNE);
    }

    if (p->flow) {
        DEBUG_ASSERT_FLOW_LOCKED(p->flow);

        /* run tx cleanup last */
        AppLayerParserTransactionsCleanup(p->flow);
        FLOWLOCK_UNLOCK(p->flow);
    }

    return TM_ECODE_OK;
}

void FlowWorkerReplaceDetectCtx(void *flow_worker, void *detect_ctx)
{
    FlowWorkerThreadData *fw = flow_worker;

    SC_ATOMIC_SET(fw->detect_thread, detect_ctx);
}

void *FlowWorkerGetDetectCtxPtr(void *flow_worker)
{
    FlowWorkerThreadData *fw = flow_worker;

    return SC_ATOMIC_GET(fw->detect_thread);
}

const char *ProfileFlowWorkerIdToString(enum ProfileFlowWorkerId fwi)
{
    switch (fwi) {
        case PROFILE_FLOWWORKER_FLOW:
            return "flow";
        case PROFILE_FLOWWORKER_STREAM:
            return "stream";
        case PROFILE_FLOWWORKER_APPLAYERUDP:
            return "app-layer";
        case PROFILE_FLOWWORKER_DETECT:
            return "detect";
        case PROFILE_FLOWWORKER_TCPPRUNE:
            return "tcp-prune";
        case PROFILE_FLOWWORKER_SIZE:
            return "size";
    }
    return "error";
}

static void FlowWorkerExitPrintStats(ThreadVars *tv, void *data)
{
    FlowWorkerThreadData *fw = data;
    OutputLoggerExitPrintStats(tv, fw->output_thread);
}

void TmModuleFlowWorkerRegister (void)
{
    tmm_modules[TMM_FLOWWORKER].name = "FlowWorker";
    tmm_modules[TMM_FLOWWORKER].ThreadInit = FlowWorkerThreadInit;
    tmm_modules[TMM_FLOWWORKER].Func = FlowWorker;
    tmm_modules[TMM_FLOWWORKER].ThreadDeinit = FlowWorkerThreadDeinit;
    tmm_modules[TMM_FLOWWORKER].ThreadExitPrintStats = FlowWorkerExitPrintStats;
    tmm_modules[TMM_FLOWWORKER].cap_flags = 0;
    tmm_modules[TMM_FLOWWORKER].flags = TM_FLAG_STREAM_TM|TM_FLAG_DETECT_TM;
}
