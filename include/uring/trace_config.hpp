#pragma once

#ifndef URING_ENABLE_TRACING
    #define URING_ENABLE_TRACING 0
#endif

#if URING_ENABLE_TRACING
    #define URING_TRACE_OP_FIELD const char* op_name = "";
    #define URING_TRACE_OP_RESET(op) (op).op_name = ""
    #define URING_TRACE_OP_ARG(name) name,
    #define URING_TRACE_OP_PARAM const char* op_name,
    #define URING_TRACE_OP_MEMBER const char* op_name_;
    #define URING_TRACE_OP_CTOR_INIT op_name_(op_name),
    #define URING_TRACE_SET_OP_NAME(op) (op)->op_name = op_name_
    #define URING_TRACE_SQE_FAST(token) ::URing::Tracer::sqe_fast((token), op_name_)
    #define URING_TRACE_SQE_SLOW(token, result) ::URing::Tracer::sqe_slow((token), (result), op_name_)
    #define URING_TRACE_SQE_FULL(token) ::URing::Tracer::sqe_full((token), op_name_)
    #define URING_TRACE_SUBMIT(token) ::URing::Tracer::submit((token), op_name_)
    #define URING_TRACE_COMPLETE(token, result, op) ::URing::Tracer::complete((token), (result), (op)->op_name)
    #define URING_TRACE_CANCEL(token) ::URing::Tracer::cancel((token))
    #define URING_TRACE_WAKE() ::URing::Tracer::wake()
    #define URING_TRACE_SPAWN_FAST() ::URing::Tracer::spawn_fast()
    #define URING_TRACE_SPAWN_SLOW() ::URing::Tracer::spawn_slow()
    #define URING_TRACE_SPAWN_FULL() ::URing::Tracer::spawn_full()
    #define URING_TRACE_ALLOC(size) ::URing::Tracer::alloc((size))
    #define URING_TRACE_FREE(size) ::URing::Tracer::free((size))
#else
    #define URING_TRACE_OP_FIELD
    #define URING_TRACE_OP_RESET(op) ((void)0)
    #define URING_TRACE_OP_ARG(name)
    #define URING_TRACE_OP_PARAM
    #define URING_TRACE_OP_MEMBER
    #define URING_TRACE_OP_CTOR_INIT
    #define URING_TRACE_SET_OP_NAME(op) ((void)0)
    #define URING_TRACE_SQE_FAST(token) ((void)0)
    #define URING_TRACE_SQE_SLOW(token, result) ((void)0)
    #define URING_TRACE_SQE_FULL(token) ((void)0)
    #define URING_TRACE_SUBMIT(token) ((void)0)
    #define URING_TRACE_COMPLETE(token, result, op) ((void)0)
    #define URING_TRACE_CANCEL(token) ((void)0)
    #define URING_TRACE_WAKE() ((void)0)
    #define URING_TRACE_SPAWN_FAST() ((void)0)
    #define URING_TRACE_SPAWN_SLOW() ((void)0)
    #define URING_TRACE_SPAWN_FULL() ((void)0)
    #define URING_TRACE_ALLOC(size) ((void)0)
    #define URING_TRACE_FREE(size) ((void)0)
#endif
