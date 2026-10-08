#pragma once
#include "rpc.h"
#ifndef __RPCNDR_H_VERSION__
#define __RPCNDR_H_VERSION__ 475
#endif
#ifndef EXTERN_C
#ifdef __cplusplus
#define EXTERN_C extern "C"
#else
#define EXTERN_C extern
#endif
#endif
#define DECLSPEC_UUID(x)
#define DECLSPEC_NOVTABLE
#define MIDL_INTERFACE(x) struct
#define BEGIN_INTERFACE
#define END_INTERFACE
typedef unsigned char byte;
typedef unsigned char boolean;
#ifdef __cplusplus
/* Proxy/stub types appear only in C-style prototypes MIDL emits. */
struct IRpcStubBuffer;
struct IRpcChannelBuffer;
#endif
#ifdef __cplusplus
extern "C" {
#endif
void *MIDL_user_allocate(size_t n);
void MIDL_user_free(void *p);
#ifdef __cplusplus
}
#endif
