#pragma once
#include "windows.h"
/* RPC/MIDL plumbing used by generated COM headers (wolapi/WOLAPI.h). */
#define __RPC_FAR
#define __RPC_API
#define __RPC_USER
#define __RPC_STUB
#define RPC_ENTRY
typedef void *RPC_IF_HANDLE;
typedef struct _RPC_MESSAGE *PRPC_MESSAGE;
typedef long RPC_STATUS;
