#undef TRACE_SYSTEM
#define TRACE_SYSTEM rowclone

#if !defined(_TRACE_ROWCLONE_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_ROWCLONE_H

#include <linux/tracepoint.h>

DECLARE_EVENT_CLASS(rowclone_template,
    
    TP_PROTO(unsigned long src, unsigned long dst),
    TP_ARGS(src, dst),

    TP_STRUCT__entry(
            __field(unsigned long, src)
            __field(unsigned long, dst)
    ),

    TP_fast_assign(
            __entry->src = src;
            __entry->dst = dst;
    ),

    TP_printk("[RC] 0x%lx 0x%lx", __entry->src, __entry->dst)

);

DEFINE_EVENT(rowclone_template, rowclone_read,

    TP_PROTO(unsigned long src, unsigned long dst),

    TP_ARGS(src, dst)
);

DEFINE_EVENT(rowclone_template, rowclone_write,

    TP_PROTO(unsigned long src, unsigned long dst),

    TP_ARGS(src, dst)
);

#endif

#include <trace/define_trace.h>