// Copyright 2016 VMware, Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.


//
// VMCI sockets communication - client side.
//
// Called mainly from Go code.
//
// API: Exposes only Vmci_GetReply. The call is blocking.
//
//

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <stdint.h>

#include "vmci_sockets.h"
#include "connection_types.h"

// operations status. 0 is OK
typedef int be_sock_status;

//
// Booking structure for opened VMCI / vSocket
//
typedef struct {
   int sock_id; // socket id for socket APIs
   struct sockaddr_vm addr; // held here for bookkeeping and reporting
} be_sock_id;

//
// Protocol message structure: request and reply
//

typedef struct be_request {
   uint32_t mlen;   // length of message (including trailing \0)
   const char *msg; // null-terminated immutable JSON string.
} be_request;

#define MAXBUF 1024 * 1024 // Safety limit. We do not expect json string > 1M

typedef struct be_answer {
   char *buf;   // calloced, so needs to be free()
} be_answer;

//
// Interface for communication to "command execution" server.
//
typedef struct be_funcs {
   const char *shortName; // name of the interaface (key to access it)
   const char *name;      // longer explanation (human help)

   // init the channel, return status and ID
   be_sock_status
   (*init_sock)(be_sock_id *id, int cid, int port);
   // release the channel - clean up
   void
   (*release_sock)(be_sock_id *id);

   // send a request and get  reply - blocking
   be_sock_status
   (*get_reply)(be_sock_id *id, be_request *r, be_answer* a);
} be_funcs;

// Vsocket communication implementation forward declarations
static be_sock_status
vsock_init(be_sock_id *id, int cid, int port);
static be_sock_status
vsock_get_reply(be_sock_id *id, be_request *r, be_answer* a);
static void
vsock_release(be_sock_id *id);

// Dummy communication declaration - dummy just prints stuff, for test
static be_sock_status
dummy_init(be_sock_id *id, int cid, int port);
static be_sock_status
dummy_get_reply(be_sock_id *id, be_request *r, be_answer* a);
static void
dummy_release(be_sock_id *id);

// support communication interfaces
#define VSOCKET_BE_NAME "vsocket" // backend to communicate via vSocket
#define ESX_VMCI_CID    2  		  // ESX host VMCI CID ("address")
#define DUMMY_BE_NAME "dummy"     // backend which only returns OK, for unit test

be_funcs backends[] =
   {
      {
      VSOCKET_BE_NAME, "vSocket Communication Backend v0.1", vsock_init,
      vsock_release, vsock_get_reply },
      {
      DUMMY_BE_NAME, "Dummy Communication Backend", dummy_init, dummy_release,
      dummy_get_reply },

      {
      0 } };


// Get backend by name
static be_funcs *
get_backend(const char *shortName)
{
   be_funcs *be = backends;
   while (be && be->shortName && *be->shortName) {
      if (strcmp(shortName, be->shortName) == 0) {
         return be;
      }
      be++;
   }
   return NULL;
}

// "dummy" interface implementation
// Used for manual testing mainly,
// to make sure data arrives to backend
//----------------------------------
static be_sock_status
dummy_init(be_sock_id *id, int cid, int port)
{
   // printf connecting
   printf("dummy_init: connected.\n");
   return CONN_SUCCESS;
}

static void
dummy_release(be_sock_id *id)
{
   printf("dummy_release: released.\n");
}

static be_sock_status
dummy_get_reply(be_sock_id *id, be_request *r, be_answer* a)
{
   printf("dummy_get_reply: got request %s.\n", r->msg);
   printf("dummy_get_reply: replying empty (for now).\n");
   a->buf = strdup("none");

   return CONN_SUCCESS;
}

// vsocket interface implementation
//---------------------------------

// Get socket family for VMCI. Returns -1 on failure
// Actually opens and keep FD to /dev/vsock to indicate
// to the kernel that VMCI driver is used by this process.
// Need to be inited once.
// Can be released explicitly on exit, or left as is and process completion
// will clean it up
static int
vsock_get_family(void)
{
   static int af = -1;

   if (af == -1) { // TODO: for multi-thread will need a lock. Issue #35
      af = VMCISock_GetAFValue();
   }
   return af;
}

// Create and connect VMCI socket.
// return success (0) or failure (-1)
static be_sock_status
vsock_init(be_sock_id *id, int cid, int port)
{
   int ret;
   int af;    // family id
   int sock;     // socket id

   if ((af = vsock_get_family()) == -1) {
      errno = EAFNOSUPPORT;
      return CONN_FAILURE;
   }
   sock = socket(af, SOCK_STREAM, 0);
   if (sock == -1) {
      return CONN_FAILURE;
   }

   id->sock_id = sock;

   // Connect to the server.
   memset(&id->addr, 0, sizeof id->addr);
   id->addr.svm_family = af;
   id->addr.svm_cid = cid;
   id->addr.svm_port = port;
   ret = connect(sock, (const struct sockaddr *) &id->addr, sizeof id->addr);
   if (ret != 0) {
      int old_errno = errno;
      vsock_release(id);
      errno = old_errno;
      return CONN_FAILURE;
   }

   return CONN_SUCCESS;
}

//
// Send request (r->msg) and wait for reply.
// returns 0 on success , -1 (or potentially errno) on error
// On success , allocates a->buf ( caller needs to free it) and placed reply there
// Expects r and a to be allocated by the caller.
//
//
static be_sock_status
vsock_get_reply(be_sock_id *s, be_request *r, be_answer* a)
{
   int ret;
   uint32_t b; // smallish buffer

   // Try to send a message to the server.
   b = MAGIC;
   ret = send(s->sock_id, &b, sizeof b, 0);
   if (ret == -1 || ret != sizeof b) {
      return CONN_FAILURE;
   }

   ret = send(s->sock_id, &r->mlen, sizeof r->mlen, 0);
   if (ret == -1 || ret != sizeof r->mlen) {
      return CONN_FAILURE;
   }

   ret = send(s->sock_id, r->msg, r->mlen, 0);
   if (ret == -1 || ret != r->mlen) {
      return CONN_FAILURE;
   }

   // Now get the reply (blocking, wait on ESX-side execution):

   // MAGIC:
   b = 0;
   ret = recv(s->sock_id, &b, sizeof b, 0);
   if (ret == -1 || ret != sizeof b ) {
      return CONN_FAILURE;
   }
   if (b != MAGIC) {
      fprintf(stderr, "Wrong magic: got 0x%x expected 0x%x\n", b, MAGIC);
      errno = EBADMSG;
      return CONN_FAILURE;
   }

   // length
   b = 0;
   ret = recv(s->sock_id, &b, sizeof b, 0);
   if (ret == -1 || ret != sizeof b) {
      fprintf(stderr, "Failed to receive len: ret %d (%s)\n", ret, strerror(errno));
      return CONN_FAILURE;
   }

   a->buf = calloc(1, b);
   if (!a->buf) {
      return CONN_FAILURE;
   }

   ret = recv(s->sock_id, a->buf, b, 0);
   if (ret == -1 || ret != b) {
      free(a->buf);
      return CONN_FAILURE;
   }

   return CONN_SUCCESS;
}

// release socket and vmci info
static void
vsock_release(be_sock_id *id)
{
   close(id->sock_id);
}

//
// Handle one request using BE interface
// Yes,  we DO create and bind socket for each request - it's management
// so we can afford overhead, and it allows connection to be stateless.
//
static be_sock_status
host_request(be_funcs *be, be_request* req, be_answer* ans, int cid, int port)
{

   int vmciFd;
   int af;
   be_sock_id id;
   be_sock_status ret;

   if ((ret = be->init_sock(&id, cid, port)) != 0) {
      return ret;
   }

   ret = be->get_reply(&id, req, ans);
   be->release_sock(&id);

   return ret;
}

//
//
// Entry point for vsocket requests.
// Returns NULL for success, -1 for err, and sets errno if needed
// <ans> is allocated upstairs
//
const be_sock_status
Vmci_GetReply(int port, const char* json_request, const char* be_name,
              be_answer* ans)
{
   be_request req;
   be_funcs *be = get_backend(be_name);

   if (be == NULL) {
      errno = ENXIO; // reusing "no such device or adress" for wrong BE name
      return CONN_FAILURE;
   }

   req.mlen = strnlen(json_request, MAXBUF) + 1;
   req.msg = json_request;

   return host_request(be, &req, ans, ESX_VMCI_CID, port);
}
