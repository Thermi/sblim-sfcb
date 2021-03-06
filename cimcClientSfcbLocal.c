
/*
 * $Id: cimcClientSfcbLocal.c,v 1.32 2009/09/01 18:59:06 buccella Exp $
 *
 * © Copyright IBM Corp. 2006, 2007
 *
 * THIS FILE IS PROVIDED UNDER THE TERMS OF THE ECLIPSE PUBLIC LICENSE
 * ("AGREEMENT"). ANY USE, REPRODUCTION OR DISTRIBUTION OF THIS FILE
 * CONSTITUTES RECIPIENTS ACCEPTANCE OF THE AGREEMENT.
 *
 * You can obtain a current copy of the Eclipse Public License from
 * http://www.opensource.org/licenses/eclipse-1.0.php
 *
 * Author:        Adrian Schuur <schuur@de.ibm.com>
 *
 * Description:
 *
 * sfcb's implementation of the sfcc CIMC API
 *
 */

#include "cimcClientSfcbLocal.h"
#include "cmpi/cmpidt.h"
#include "cmpi/cmpift.h"
#include "cmpi/cmpimacs.h"
#include "mlog.h"
#include <syslog.h>
#include "control.h"
#include "objectImpl.h"

#include "providerMgr.h"
#include "queryOperation.h"

#define CIMCSetStatusWithChars(st,rcp,chars) \
      { (st)->rc=(rcp); \
        (st)->msg=NewCMPIString((chars),NULL); }

//#define NewCMPIString sfcb_native_new_CMPIString

extern unsigned long _sfcb_trace_mask;

extern void     closeProviderContext(BinRequestContext * binCtx);
extern MsgSegment setObjectPathMsgSegment(const CMPIObjectPath * op);
extern MsgSegment setArgsMsgSegment(CMPIArgs * args);
extern MsgSegment setInstanceMsgSegment(const CMPIInstance *inst);
extern CMPIConstClass *relocateSerializedConstClass(void *area);
extern CMPIObjectPath *relocateSerializedObjectPath(void *area);
extern CMPIInstance *relocateSerializedInstance(void *area);
extern CMPIArgs *relocateSerializedArgs(void *area);

extern CMPIObjectPath *NewCMPIObjectPath(const char *ns, const char *cn,
                                         CMPIStatus *);
extern CMPIInstance *NewCMPIInstance(CMPIObjectPath * cop, CMPIStatus *rc);
extern CMPIArray *NewCMPIArray(CMPICount size, CMPIType type,
                               CMPIStatus *rc);
extern CMPIEnumeration *sfcb_native_new_CMPIEnumeration(CMPIArray *array,
                                                        CMPIStatus *rc);
extern CMPIString *NewCMPIString(const char *ptr, CMPIStatus *rc);
extern CMPIEnumeration *NewCMPIEnumeration(CMPIArray *array,
                                           CMPIStatus *rc);

extern CMPIArgs *NewCMPIArgs(CMPIStatus *rc);
extern CMPIDateTime *NewCMPIDateTime(CMPIStatus *rc);
extern CMPIDateTime *NewCMPIDateTimeFromBinary(CMPIUint64 binTime,
                                               CMPIBoolean interval,
                                               CMPIStatus *rc);
extern CMPIDateTime *NewCMPIDateTimeFromChars(const char *utcTime,
                                              CMPIStatus *rc);

extern void     setInstanceLocalMode(int mode);

extern int      localClientMode;

extern void     sunsetControl();

int             httpProcIdX;
long            httpReqHandlerTimeout;

#include <stdlib.h>
#include <string.h>

#ifdef DEBUG
#undef DEBUG
#define DEBUG 1
#else
#define DEBUG 0
#endif

#ifdef DMALLOC
#include "dmalloc.h"
#endif

#if DEBUG
int             do_debug = 0;   /* simple enable debug messages flag */

static void
set_debug()
{
  static int      firsttime = 1;
  char           *dbg;
  if (firsttime == 1) {
    firsttime--;
    do_debug = ((dbg = getenv("CMPISFCC_DEBUG")) != NULL &&
                strcasecmp(dbg, "true") == 0);
  }
}
#endif

typedef const struct _ClientConnectionFT {
  CMPIStatus      (*release) (ClientConnection *);
} ClientConnectionFT;

struct _ClientEnc {
  Client          enc;
  ClientData      data;
  CredentialData  certData;
  ClientConnection *connection;
};

struct _ClientConnection {
  ClientConnectionFT *ft;
};

char           *getResponse(ClientConnection *con, CMPIObjectPath * cop);
ClientConnection *initConnection(ClientData * cld);

static CMPIConstClass * getClass(Client * ,CMPIObjectPath * ,CMPIFlags ,char ** ,CMPIStatus * );

/*
 * --------------------------------------------------------------------------
 */

static CMPIStatus
releaseConnection(ClientConnection *con)
{
  CMPIStatus      rc = { CMPI_RC_OK, NULL };
  free(con);
  con = NULL;
  return rc;
}

static ClientConnectionFT conFt = {
  releaseConnection
};

/*
 * --------------------------------------------------------------------------
 */

/* cld isn't used, but we need to keep it around for now.
   need to orchestrate the change with sfcc */

ClientConnection *
initConnection(ClientData __attribute__ ((unused)) *cld)
{
  ClientConnection *c = calloc(1, sizeof(*c));
  c->ft = &conFt;
  return c;
}

/*--------------------------------------------------------------------------*/
/*
 * --------------------------------------------------------------------------
 */

static Client  *
cloneClient(Client __attribute__ ((unused)) *cl, CMPIStatus *st)
{
  CIMCSetStatusWithChars(st, CMPI_RC_ERR_NOT_SUPPORTED,
                         "Clone function not supported");
  return NULL;
}

/* releases the Client created in CMPIConnect2() */
static CMPIStatus
releaseClient(Client * mb)
{
  CMPIStatus      rc = { CMPI_RC_OK, NULL };
  ClientEnc      *cl = (ClientEnc *) mb;

  if (cl->data.hostName) {
    free(cl->data.hostName);
  }
  if (cl->data.user) {
    free(cl->data.user);
  }
  if (cl->data.pwd) {
    free(cl->data.pwd);
  }
  if (cl->data.scheme) {
    free(cl->data.scheme);
  }
  if (cl->data.port) {
    free(cl->data.port);
  }
  if (cl->certData.trustStore) {
    free(cl->certData.trustStore);
  }
  if (cl->certData.certFile) {
    free(cl->certData.certFile);
  }
  if (cl->certData.keyFile) {
    free(cl->certData.keyFile);
  }

  if (cl->connection)
    CMRelease(cl->connection);

  free(cl);
  cl = NULL;
  return rc;
}

void
freeResps(BinResponseHdr ** resp, int count)
{
  if (resp && count)
    while (count)
      free(resp[(count--) - 1]);
  if (resp)
    free(resp);
}

extern void     setEnumArray(CMPIEnumeration *enumeration,
                             CMPIArray *array);
static CMPIEnumeration *
cpyEnumResponses(BinRequestContext * binCtx,
                 BinResponseHdr ** resp, int arrLen)
{
  unsigned long   i,
                  j,
                  c;
  union o {
    CMPIInstance   *inst;
    CMPIObjectPath *path;
    CMPIConstClass *cls;
  } object;
  CMPIArray      *ar,
                 *art;
  CMPIEnumeration *enm;

  _SFCB_ENTER(TRACE_CIMXMLPROC, "genEnumResponses");

  ar = NewCMPIArray(arrLen, binCtx->type, NULL);
  art = NewCMPIArray(0, binCtx->type, NULL);

  for (c = 0, i = 0; i < binCtx->rCount; i++) {
    for (j = 0; j < resp[i]->count; c++, j++) {
      if (binCtx->type == CMPI_ref)
        object.path =
            relocateSerializedObjectPath(resp[i]->object[j].data);
      else if (binCtx->type == CMPI_instance)
        object.inst = relocateSerializedInstance(resp[i]->object[j].data);
      else if (binCtx->type == CMPI_class) {
        object.cls = relocateSerializedConstClass(resp[i]->object[j].data);
      }
      // object.inst=CMClone(object.inst,NULL);
      CMSetArrayElementAt(ar, c, (CMPIValue *) & object.inst,
			  binCtx->type);
    }
  }

  enm = NewCMPIEnumeration(art, NULL);
  setEnumArray(enm, ar);
  art->ft->release(art);

  _SFCB_RETURN(enm);
}

static void
ctxErrResponse(BinRequestContext * ctx, CMPIStatus *rc)
{
  MsgXctl        *xd = ctx->ctlXdata;
  char            msg[256],
                 *m;
  int             r = CMPI_RC_ERR_FAILED;

  switch (ctx->rc) {
  case MSG_X_NOT_SUPPORTED:
    m = "Operation not supported yy";
    r = CMPI_RC_ERR_NOT_SUPPORTED;
    break;
  case MSG_X_INVALID_CLASS:
    m = "Class not found";
    r = CMPI_RC_ERR_INVALID_CLASS;
    break;
  case MSG_X_INVALID_NAMESPACE:
    m = "Invalid namespace";
    r = CMPI_RC_ERR_INVALID_NAMESPACE;
    break;
  case MSG_X_PROVIDER_NOT_FOUND:
    m = "Provider not found or not loadable";
    r = CMPI_RC_ERR_NOT_FOUND;
    break;
  case MSG_X_FAILED:
    m = xd->data;
    break;
  default:
    sprintf(msg, "Internal error - %d\n", ctx->rc);
    m = msg;
  }

  if (rc)
    CIMCSetStatusWithChars(rc, r, m);
}

static void
closeSockets(BinRequestContext * binCtx)
{
  unsigned long c;
  for (c = 0; c < binCtx->pCount; c++) {
    close(binCtx->pAs[c].socket);
  }
}

static CMPIEnumeration *
enumInstanceNames(Client * mb, CMPIObjectPath * cop, CMPIStatus *rc)
{
  _SFCB_ENTER(TRACE_CIMXMLPROC, "enumInstanceNames");
  EnumInstanceNamesReq sreq = BINREQ(OPS_EnumerateInstanceNames, EIN_REQ_REG_SEGMENTS);
  int             irc,
                  l = 0,
      err = 0;
  BinResponseHdr **resp;
  BinRequestContext binCtx;
  OperationHdr    oHdr = { OPS_EnumerateInstanceNames, 0, 2 };
  CMPIEnumeration *enm = NULL;

  if (rc)
    CMSetStatus(rc, CMPI_RC_OK);

  CMPIString     *ns = cop->ft->getNameSpace(cop, NULL);
  CMPIString     *cn = cop->ft->getClassName(cop, NULL);

  oHdr.nameSpace = setCharsMsgSegment((char *) ns->hdl);
  oHdr.className = setCharsMsgSegment((char *) cn->hdl);

  memset(&binCtx, 0, sizeof(BinRequestContext));

  sreq.objectPath = setObjectPathMsgSegment(cop);
  sreq.principal = setCharsMsgSegment(((ClientEnc *) mb)->data.user);
  sreq.userRole = setCharsMsgSegment(NULL);

  binCtx.oHdr = (OperationHdr *) & oHdr;
  binCtx.bHdr = &sreq.hdr;
  binCtx.bHdr->flags = 0;
  binCtx.type = CMPI_ref;
  binCtx.rHdr = NULL;
  binCtx.bHdrSize = sizeof(sreq);
  binCtx.chunkedMode = binCtx.xmlAs = binCtx.noResp = 0;
  binCtx.pAs = NULL;

  _SFCB_TRACE(1, ("--- Getting Provider context"));
  irc = getProviderContext(&binCtx);
  CMRelease(ns);
  CMRelease(cn);

  if (irc == MSG_X_PROVIDER) {
    _SFCB_TRACE(1, ("--- Calling Providers"));
    resp = invokeProviders(&binCtx, &err, &l);
    _SFCB_TRACE(1, ("--- Back from Provider"));

    closeSockets(&binCtx);
    closeProviderContext(&binCtx);

    if (err == 0) {
      enm = cpyEnumResponses(&binCtx, resp, l);
      freeResps(resp, binCtx.pCount);
      _SFCB_RETURN(enm);
    }
    if (rc)
      CIMCSetStatusWithChars(rc, resp[err - 1]->rc,
                             (char *) resp[err - 1]->object[0].data);
    if (resp)
      freeResps(resp, binCtx.pCount);
    _SFCB_RETURN(NULL);
  } else
    ctxErrResponse(&binCtx, rc);
  closeProviderContext(&binCtx);

  _SFCB_RETURN(NULL);
}

static CMPIEnumeration *
enumInstances(Client * mb,
              CMPIObjectPath * cop,
              CMPIFlags flags, char **properties, CMPIStatus *rc)
{
  EnumInstancesReq *sreq;
  int             pCount = 0,
      irc,
      l = 0,
      err = 0,
      sreqSize = sizeof(EnumInstancesReq);
  BinResponseHdr **resp;
  BinRequestContext binCtx;
  OperationHdr    oHdr = { OPS_EnumerateInstances, 0, 2 };
  CMPIEnumeration *enm = NULL;

  _SFCB_ENTER(TRACE_CIMXMLPROC, "enumInstances");

  if (rc)
    CMSetStatus(rc, CMPI_RC_OK);

  CMPIString     *ns = cop->ft->getNameSpace(cop, NULL);
  CMPIString     *cn = cop->ft->getClassName(cop, NULL);

  oHdr.nameSpace = setCharsMsgSegment((char *) ns->hdl);
  oHdr.className = setCharsMsgSegment((char *) cn->hdl);

  memset(&binCtx, 0, sizeof(BinRequestContext));

  if (properties) {
    char          **p;
    for (p = properties; *p; p++)
      pCount++;
  }

  sreqSize += pCount * sizeof(MsgSegment);
  sreq = calloc(1, sreqSize);
  sreq->hdr.operation = OPS_EnumerateInstances;
  sreq->hdr.count = pCount + EI_REQ_REG_SEGMENTS;

  sreq->objectPath = setObjectPathMsgSegment(cop);
  sreq->principal = setCharsMsgSegment(((ClientEnc *) mb)->data.user);
  sreq->userRole = setCharsMsgSegment(NULL);

  binCtx.oHdr = (OperationHdr *) & oHdr;
  binCtx.bHdr = &sreq->hdr;
  binCtx.bHdr->flags = flags;
  binCtx.type = CMPI_instance;
  binCtx.rHdr = NULL;
  binCtx.bHdrSize = sreqSize;
  binCtx.chunkedMode = binCtx.xmlAs = binCtx.noResp = 0;
  binCtx.pAs = NULL;

  _SFCB_TRACE(1, ("--- Getting Provider context"));
  irc = getProviderContext(&binCtx);

  CMRelease(ns);
  CMRelease(cn);

  if (irc == MSG_X_PROVIDER) {
    _SFCB_TRACE(1, ("--- Calling Providers"));
    resp = invokeProviders(&binCtx, &err, &l);

    closeSockets(&binCtx);
    closeProviderContext(&binCtx);

    if (err == 0) {
      enm = cpyEnumResponses(&binCtx, resp, l);
      freeResps(resp, binCtx.pCount);
      free(sreq);
      _SFCB_RETURN(enm);
    }
    if (rc)
      CIMCSetStatusWithChars(rc, resp[err - 1]->rc,
                             (char *) resp[err - 1]->object[0].data);
    if (resp)
      freeResps(resp, binCtx.pCount);
    free(sreq);
    _SFCB_RETURN(NULL);
  } else
    ctxErrResponse(&binCtx, rc);
  closeProviderContext(&binCtx);
  free(sreq);

  _SFCB_RETURN(NULL);

}

/*
 * --------------------------------------------------------------------------
 */

static CMPIInstance *
getInstance(Client * mb,
            CMPIObjectPath * cop,
            CMPIFlags flags, char **properties, CMPIStatus *rc)
{
  CMPIInstance   *inst;
  int             irc,
                  i,
                  sreqSize = sizeof(GetInstanceReq) - sizeof(MsgSegment);
  BinRequestContext binCtx;
  BinResponseHdr *resp;
  GetInstanceReq *sreq;
  int             pCount = 0;
  OperationHdr    oHdr = { OPS_GetInstance, 0, 2 };

  _SFCB_ENTER(TRACE_CIMXMLPROC, "getInstance");

  if (rc)
    CMSetStatus(rc, CMPI_RC_OK);

  CMPIString     *ns = cop->ft->getNameSpace(cop, NULL);
  CMPIString     *cn = cop->ft->getClassName(cop, NULL);

  oHdr.nameSpace = setCharsMsgSegment((char *) ns->hdl);
  oHdr.className = setCharsMsgSegment((char *) cn->hdl);

  memset(&binCtx, 0, sizeof(BinRequestContext));

  if (properties) {
    char          **p;
    for (p = properties; *p; p++)
      pCount++;
  }

  sreqSize += pCount * sizeof(MsgSegment);
  sreq = calloc(1, sreqSize);
  sreq->hdr.operation = OPS_GetInstance;
  sreq->hdr.count = pCount + GI_REQ_REG_SEGMENTS;

  sreq->objectPath = setObjectPathMsgSegment(cop);
  sreq->principal = setCharsMsgSegment(((ClientEnc *) mb)->data.user);
  sreq->userRole = setCharsMsgSegment(NULL);

  for (i = 0; i < pCount; i++)
    sreq->properties[i] = setCharsMsgSegment(properties[i]);

  binCtx.oHdr = (OperationHdr *) & oHdr;
  binCtx.bHdr = &sreq->hdr;
  binCtx.bHdr->flags = flags;
  binCtx.rHdr = NULL;
  binCtx.bHdrSize = sreqSize;
  binCtx.chunkedMode = binCtx.xmlAs = binCtx.noResp = 0;
  binCtx.pAs = NULL;

  _SFCB_TRACE(1, ("--- Getting Provider context"));
  irc = getProviderContext(&binCtx);

  CMRelease(ns);
  CMRelease(cn);

  if (irc == MSG_X_PROVIDER) {
    _SFCB_TRACE(1, ("--- Calling Provider"));
    resp = invokeProvider(&binCtx);

    closeSockets(&binCtx);
    closeProviderContext(&binCtx);

    resp->rc--;
    if (resp->rc == CMPI_RC_OK) {
      inst = relocateSerializedInstance(resp->object[0].data);
      inst = inst->ft->clone(inst, NULL);
      free(sreq);
      free(resp);
      _SFCB_RETURN(inst);
    }
    free(sreq);
    if (rc)
      CIMCSetStatusWithChars(rc, resp->rc, (char *) resp->object[0].data);
    free(resp);
    _SFCB_RETURN(NULL);
  } else
    ctxErrResponse(&binCtx, rc);
  free(sreq);
  closeProviderContext(&binCtx);

  _SFCB_RETURN(NULL);
}

/*
 * --------------------------------------------------------------------------
 */

static CMPIObjectPath *
createInstance(Client * mb,
               CMPIObjectPath * cop, CMPIInstance *inst, CMPIStatus *rc)
{
  int             irc;
  BinRequestContext binCtx;
  BinResponseHdr *resp;
  CreateInstanceReq sreq = BINREQ(OPS_CreateInstance, CI_REQ_REG_SEGMENTS);
  CMPIObjectPath *path;
  OperationHdr    oHdr = { OPS_CreateInstance, 0, 3 };

  _SFCB_ENTER(TRACE_CIMXMLPROC, "createInst");

  if (rc)
    CMSetStatus(rc, CMPI_RC_OK);

  CMPIString     *ns = cop->ft->getNameSpace(cop, NULL);
  CMPIString     *cn = cop->ft->getClassName(cop, NULL);

  oHdr.nameSpace = setCharsMsgSegment((char *) ns->hdl);
  oHdr.className = setCharsMsgSegment((char *) cn->hdl);

  memset(&binCtx, 0, sizeof(BinRequestContext));

  sreq.principal = setCharsMsgSegment(((ClientEnc *) mb)->data.user);
  sreq.userRole = setCharsMsgSegment(NULL);
  sreq.path = setObjectPathMsgSegment(cop);
  sreq.instance = setInstanceMsgSegment(inst);

  binCtx.oHdr = (OperationHdr *) & oHdr;
  binCtx.bHdr = &sreq.hdr;
  binCtx.rHdr = NULL;
  binCtx.bHdrSize = sizeof(sreq);
  binCtx.chunkedMode = binCtx.xmlAs = binCtx.noResp = 0;
  binCtx.pAs = NULL;

  _SFCB_TRACE(1, ("--- Getting Provider context"));
  irc = getProviderContext(&binCtx);

  CMRelease(ns);
  CMRelease(cn);

  if (irc == MSG_X_PROVIDER) {
    _SFCB_TRACE(1, ("--- Calling Provider"));
    resp = invokeProvider(&binCtx);

    closeSockets(&binCtx);
    closeProviderContext(&binCtx);

    resp->rc--;
    if (resp->rc == CMPI_RC_OK) {
      path = relocateSerializedObjectPath(resp->object[0].data);
      path = path->ft->clone(path, NULL);
      free(resp);
      _SFCB_RETURN(path);
    }
    if (rc)
      CIMCSetStatusWithChars(rc, resp->rc, (char *) resp->object[0].data);
    free(resp);
    _SFCB_RETURN(NULL);
  } else
    ctxErrResponse(&binCtx, rc);
  closeProviderContext(&binCtx);

  _SFCB_RETURN(NULL);
}

/*
 * --------------------------------------------------------------------------
 */

static CMPIStatus
modifyInstance(Client * mb,
               CMPIObjectPath * cop,
               CMPIInstance *inst, CMPIFlags __attribute__ ((unused)) flags, char **properties)
{
  int             pCount = 0,
      irc,
      i,
      sreqSize = sizeof(ModifyInstanceReq) - sizeof(MsgSegment);
  BinRequestContext binCtx;
  BinResponseHdr *resp;
  ModifyInstanceReq *sreq;
  OperationHdr    oHdr = { OPS_ModifyInstance, 0, 2 };
  CMPIStatus      rc = { 0, NULL };

  _SFCB_ENTER(TRACE_CIMXMLPROC, "setInstance");

  CMPIString     *ns = cop->ft->getNameSpace(cop, NULL);
  CMPIString     *cn = cop->ft->getClassName(cop, NULL);

  oHdr.nameSpace = setCharsMsgSegment((char *) ns->hdl);
  oHdr.className = setCharsMsgSegment((char *) cn->hdl);

  memset(&binCtx, 0, sizeof(BinRequestContext));

  if (properties) {
    char          **p;
    for (p = properties; *p; p++)
      pCount++;
  }

  sreqSize += pCount * sizeof(MsgSegment);
  sreq = calloc(1, sreqSize);

  for (i = 0; i < pCount; i++)
    sreq->properties[i] = setCharsMsgSegment(properties[i]);

  sreq->hdr.operation = OPS_ModifyInstance;
  sreq->hdr.count = pCount + MI_REQ_REG_SEGMENTS;

  sreq->instance = setInstanceMsgSegment(inst);
  sreq->path = setObjectPathMsgSegment(cop);
  sreq->principal = setCharsMsgSegment(((ClientEnc *) mb)->data.user);
  sreq->userRole = setCharsMsgSegment(NULL);

  binCtx.oHdr = (OperationHdr *) & oHdr;
  binCtx.bHdr = &sreq->hdr;
  binCtx.rHdr = NULL;
  binCtx.bHdrSize = sreqSize;
  binCtx.chunkedMode = binCtx.xmlAs = binCtx.noResp = 0;
  binCtx.pAs = NULL;

  _SFCB_TRACE(1, ("--- Getting Provider context"));
  irc = getProviderContext(&binCtx);

  CMRelease(ns);
  CMRelease(cn);

  if (irc == MSG_X_PROVIDER) {
    _SFCB_TRACE(1, ("--- Calling Provider"));
    resp = invokeProvider(&binCtx);

    closeSockets(&binCtx);
    closeProviderContext(&binCtx);

    resp->rc--;
    if (resp->rc == CMPI_RC_OK) {
      free(sreq);
      free(resp);
      _SFCB_RETURN(rc);
    }
    free(sreq);
    CIMCSetStatusWithChars(&rc, resp->rc, (char *) resp->object[0].data);
    free(resp);
    _SFCB_RETURN(rc);
  } else
    ctxErrResponse(&binCtx, &rc);
  free(sreq);
  closeProviderContext(&binCtx);

  _SFCB_RETURN(rc);
}

static CMPIStatus
deleteInstance(Client * mb, CMPIObjectPath * cop)
{
  int             irc;
  BinRequestContext binCtx;
  BinResponseHdr *resp;
  DeleteInstanceReq sreq = BINREQ(OPS_DeleteInstance, DI_REQ_REG_SEGMENTS);
  OperationHdr    oHdr = { OPS_DeleteInstance, 0, 2 };
  CMPIStatus      rc = { 0, NULL };

  _SFCB_ENTER(TRACE_CIMXMLPROC, "deleteInstance");

  CMPIString     *ns = cop->ft->getNameSpace(cop, NULL);
  CMPIString     *cn = cop->ft->getClassName(cop, NULL);

  oHdr.nameSpace = setCharsMsgSegment((char *) ns->hdl);
  oHdr.className = setCharsMsgSegment((char *) cn->hdl);

  memset(&binCtx, 0, sizeof(BinRequestContext));

  sreq.objectPath = setObjectPathMsgSegment(cop);
  sreq.principal = setCharsMsgSegment(((ClientEnc *) mb)->data.user);
  sreq.userRole = setCharsMsgSegment(NULL);

  binCtx.oHdr = (OperationHdr *) & oHdr;
  binCtx.bHdr = &sreq.hdr;
  binCtx.rHdr = NULL;
  binCtx.bHdrSize = sizeof(sreq);
  binCtx.chunkedMode = binCtx.xmlAs = binCtx.noResp = 0;
  binCtx.pAs = NULL;

  _SFCB_TRACE(1, ("--- Getting Provider context"));
  irc = getProviderContext(&binCtx);

  CMRelease(ns);
  CMRelease(cn);

  if (irc == MSG_X_PROVIDER) {
    _SFCB_TRACE(1, ("--- Calling Provider"));
    resp = invokeProvider(&binCtx);

    closeSockets(&binCtx);
    closeProviderContext(&binCtx);

    resp->rc--;
    if (resp->rc == CMPI_RC_OK) {
      free(resp);
      _SFCB_RETURN(rc);
    }
    CIMCSetStatusWithChars(&rc, resp->rc, (char *) resp->object[0].data);
    free(resp);
    _SFCB_RETURN(rc);
  } else
    ctxErrResponse(&binCtx, &rc);
  closeProviderContext(&binCtx);

  _SFCB_RETURN(rc);
}

static CMPIEnumeration *
execQuery(Client * mb,
          CMPIObjectPath * cop,
          const char *query, const char *lang, CMPIStatus *rc)
{
  ExecQueryReq    sreq = BINREQ(OPS_ExecQuery, EQ_REQ_REG_SEGMENTS);
  int             irc,
                  l = 0,
      err = 0;
  BinResponseHdr **resp;
  BinRequestContext binCtx;
  QLStatement    *qs = NULL;
  char          **fCls;
  OperationHdr    oHdr = { OPS_ExecQuery, 0, 4 };
  CMPIObjectPath *path;
  CMPIEnumeration *enm;

  _SFCB_ENTER(TRACE_CIMXMLPROC, "execQuery");

  if (rc)
    CMSetStatus(rc, CMPI_RC_OK);

  CMPIString     *ns = cop->ft->getNameSpace(cop, NULL);

  oHdr.nameSpace = setCharsMsgSegment((char *) ns->hdl);
  qs = parseQuery(MEM_TRACKED, query, lang, NULL, NULL, &irc);

  if (irc) {
    if (rc)
      CIMCSetStatusWithChars(rc, CMPI_RC_ERR_INVALID_QUERY,
        "syntax error in query.");
     _SFCB_RETURN(NULL);
  }

  fCls = qs->ft->getFromClassList(qs);
  if (fCls == NULL || *fCls == NULL) {
    mlogf(M_ERROR, M_SHOW, "--- from clause missing\n");
    if (rc)
      CIMCSetStatusWithChars(rc, CMPI_RC_ERR_INVALID_QUERY,
       "required from clause is missing.");
    _SFCB_RETURN(NULL);
  }
  oHdr.className = setCharsMsgSegment(*fCls);

  path = NewCMPIObjectPath((char *) ns->hdl, *fCls, NULL);

  memset(&binCtx, 0, sizeof(BinRequestContext));

  sreq.objectPath = setObjectPathMsgSegment(path);
  sreq.principal = setCharsMsgSegment(((ClientEnc *) mb)->data.user);
  sreq.userRole = setCharsMsgSegment(NULL);
  sreq.query = setCharsMsgSegment(query);
  sreq.queryLang = setCharsMsgSegment(lang);

  binCtx.oHdr = (OperationHdr *) & oHdr;
  binCtx.bHdr = &sreq.hdr;
  binCtx.bHdr->flags = 0;
  binCtx.rHdr = NULL;
  binCtx.type = CMPI_instance;
  binCtx.bHdrSize = sizeof(sreq);
  binCtx.chunkedMode = binCtx.xmlAs = binCtx.noResp = 0;
  binCtx.pAs = NULL;

  _SFCB_TRACE(1, ("--- Getting Provider context"));
  irc = getProviderContext(&binCtx);
  CMRelease(ns);

  if (irc == MSG_X_PROVIDER) {
    _SFCB_TRACE(1, ("--- Calling Providers"));
    resp = invokeProviders(&binCtx, &err, &l);

    closeSockets(&binCtx);
    closeProviderContext(&binCtx);

    if (err == 0) {
      enm = cpyEnumResponses(&binCtx, resp, l);
      freeResps(resp, binCtx.pCount);
      CMRelease(path);
      _SFCB_RETURN(enm);
    }
    if (rc)
      CIMCSetStatusWithChars(rc, resp[err - 1]->rc,
                             (char *) resp[err - 1]->object[0].data);
    CMRelease(path);
    freeResps(resp, binCtx.pCount);
    _SFCB_RETURN(NULL);
  } else
    ctxErrResponse(&binCtx, rc);
  closeProviderContext(&binCtx);

  CMRelease(path);
  _SFCB_RETURN(NULL);
}

static CMPIEnumeration *
associators(Client * mb,
            CMPIObjectPath * cop,
            const char *assocClass,
            const char *resultClass,
            const char *role,
            const char *resultRole,
            CMPIFlags flags, char **properties, CMPIStatus *rc)
{
  AssociatorsReq *sreq;
  int             irc,
                  pCount = 0,
      i,
      l = 0,
      err = 0,
      sreqSize = sizeof(AssociatorsReq) - sizeof(MsgSegment);
  BinResponseHdr **resp;
  BinRequestContext binCtx;
  OperationHdr    oHdr = { OPS_Associators, 0, 6 };
  CMPIEnumeration *enm = NULL;

  _SFCB_ENTER(TRACE_CIMXMLPROC, "associators");

  if (rc)
    CMSetStatus(rc, CMPI_RC_OK);

  CMPIString     *ns = cop->ft->getNameSpace(cop, NULL);
  CMPIString     *cn = cop->ft->getClassName(cop, NULL);

  oHdr.nameSpace = setCharsMsgSegment((char *) ns->hdl);
  oHdr.className = setCharsMsgSegment((char *) cn->hdl);

  if (properties) {
    char          **p;
    for (p = properties; *p; p++)
      pCount++;
  }

  memset(&binCtx, 0, sizeof(BinRequestContext));

  if (pCount)
    sreqSize += pCount * sizeof(MsgSegment);
  sreq = calloc(1, sreqSize);
  sreq->hdr.operation = OPS_Associators;
  sreq->hdr.count = pCount + AI_REQ_REG_SEGMENTS;

  sreq->objectPath = setObjectPathMsgSegment(cop);
  sreq->resultClass = setCharsMsgSegment(resultClass);
  sreq->role = setCharsMsgSegment(role);
  sreq->assocClass = setCharsMsgSegment(assocClass);
  sreq->resultRole = setCharsMsgSegment(resultRole);
  sreq->hdr.flags = flags;
  sreq->principal = setCharsMsgSegment(((ClientEnc *) mb)->data.user);
  sreq->userRole = setCharsMsgSegment(NULL);

  for (i = 0; i < pCount; i++)
    sreq->properties[i] = setCharsMsgSegment(properties[i]);

  oHdr.className = sreq->assocClass;

  binCtx.oHdr = (OperationHdr *) & oHdr;
  binCtx.bHdr = &sreq->hdr;
  binCtx.bHdr->flags = flags;
  binCtx.rHdr = NULL;
  binCtx.bHdrSize = sreqSize;
  binCtx.type = CMPI_instance;
  binCtx.chunkedMode = binCtx.xmlAs = binCtx.noResp = 0;
  binCtx.pAs = NULL;

  _SFCB_TRACE(1, ("--- Getting Provider context"));
  irc = getProviderContext(&binCtx);

  CMRelease(ns);
  CMRelease(cn);

  if (irc == MSG_X_PROVIDER) {
    _SFCB_TRACE(1, ("--- Calling Provider"));
    resp = invokeProviders(&binCtx, &err, &l);

    closeSockets(&binCtx);
    closeProviderContext(&binCtx);

    if (err == 0) {
      enm = cpyEnumResponses(&binCtx, resp, l);
      freeResps(resp, binCtx.pCount);
      free(sreq);
      _SFCB_RETURN(enm);
    }
    if (rc)
      CIMCSetStatusWithChars(rc, resp[err - 1]->rc,
                             (char *) resp[err - 1]->object[0].data);
    if (resp)
      freeResps(resp, binCtx.pCount);
    free(sreq);
    _SFCB_RETURN(NULL);
  } else
    ctxErrResponse(&binCtx, rc);
  free(sreq);
  closeProviderContext(&binCtx);

  _SFCB_RETURN(NULL);
}

static CMPIEnumeration *
references(Client * mb,
           CMPIObjectPath * cop,
           const char *resultClass,
           const char *role,
           CMPIFlags flags, char **properties, CMPIStatus *rc)
{
  AssociatorsReq *sreq;
  int             irc,
                  pCount = 0,
      i,
      l = 0,
      err = 0,
      sreqSize = sizeof(AssociatorsReq) - sizeof(MsgSegment);
  BinResponseHdr **resp;
  BinRequestContext binCtx;
  OperationHdr    oHdr = { OPS_References, 0, 4 };
  CMPIEnumeration *enm = NULL;

  _SFCB_ENTER(TRACE_CIMXMLPROC, "references");

  if (rc)
    CMSetStatus(rc, CMPI_RC_OK);

  memset(&binCtx, 0, sizeof(BinRequestContext));

  CMPIString     *ns = cop->ft->getNameSpace(cop, NULL);
  CMPIString     *cn = cop->ft->getClassName(cop, NULL);

  oHdr.nameSpace = setCharsMsgSegment((char *) ns->hdl);
  oHdr.className = setCharsMsgSegment((char *) cn->hdl);

  if (properties) {
    char          **p;
    for (p = properties; *p; p++)
      pCount++;
  }

  sreqSize += pCount * sizeof(MsgSegment);
  sreq = calloc(1, sreqSize);
  sreq->hdr.operation = OPS_References;
  sreq->hdr.count = pCount + RI_REQ_REG_SEGMENTS;

  sreq->objectPath = setObjectPathMsgSegment(cop);
  sreq->resultClass = setCharsMsgSegment(resultClass);
  sreq->role = setCharsMsgSegment(role);
  sreq->hdr.flags = flags;
  sreq->principal = setCharsMsgSegment(((ClientEnc *) mb)->data.user);
  sreq->userRole = setCharsMsgSegment(NULL);

  for (i = 0; i < pCount; i++)
    sreq->properties[i] = setCharsMsgSegment(properties[i]);

  oHdr.className = sreq->resultClass;

  binCtx.oHdr = (OperationHdr *) & oHdr;
  binCtx.bHdr = &sreq->hdr;
  binCtx.bHdr->flags = flags;
  binCtx.rHdr = NULL;
  binCtx.bHdrSize = sreqSize;
  binCtx.type = CMPI_instance;
  binCtx.chunkedMode = binCtx.xmlAs = binCtx.noResp = 0;
  binCtx.pAs = NULL;

  _SFCB_TRACE(1, ("--- Getting Provider context"));
  irc = getProviderContext(&binCtx);

  CMRelease(ns);
  CMRelease(cn);

  if (irc == MSG_X_PROVIDER) {
    _SFCB_TRACE(1, ("--- Calling Provider"));
    resp = invokeProviders(&binCtx, &err, &l);

    closeSockets(&binCtx);
    closeProviderContext(&binCtx);

    if (err == 0) {
      enm = cpyEnumResponses(&binCtx, resp, l);
      freeResps(resp, binCtx.pCount);
      free(sreq);
      _SFCB_RETURN(enm);
    }
    if (rc)
      CIMCSetStatusWithChars(rc, resp[err - 1]->rc,
                             (char *) resp[err - 1]->object[0].data);
    if (resp)
      freeResps(resp, binCtx.pCount);
    free(sreq);
    _SFCB_RETURN(NULL);
  } else
    ctxErrResponse(&binCtx, rc);
  free(sreq);
  closeProviderContext(&binCtx);

  _SFCB_RETURN(NULL);
}

static CMPIEnumeration *
associatorNames(Client * mb,
                CMPIObjectPath * cop,
                const char *assocClass,
                const char *resultClass,
                const char *role, const char *resultRole, CMPIStatus *rc)
{
  AssociatorNamesReq sreq = BINREQ(OPS_AssociatorNames, AIN_REQ_REG_SEGMENTS);
  int             irc,
                  l = 0,
      err = 0;
  BinResponseHdr **resp;
  BinRequestContext binCtx;
  OperationHdr    oHdr = { OPS_AssociatorNames, 0, 6 };
  CMPIEnumeration *enm = NULL;

  _SFCB_ENTER(TRACE_CIMXMLPROC, "associatorNames");

  if (rc)
    CMSetStatus(rc, CMPI_RC_OK);

  CMPIString     *ns = cop->ft->getNameSpace(cop, NULL);

  memset(&binCtx, 0, sizeof(BinRequestContext));

  sreq.objectPath = setObjectPathMsgSegment(cop);

  sreq.resultClass = setCharsMsgSegment(resultClass);
  sreq.role = setCharsMsgSegment(role);
  sreq.assocClass = setCharsMsgSegment(assocClass);
  sreq.resultRole = setCharsMsgSegment(resultRole);
  sreq.principal = setCharsMsgSegment(((ClientEnc *) mb)->data.user);
  sreq.userRole = setCharsMsgSegment(NULL);

  oHdr.nameSpace = setCharsMsgSegment((char *) ns->hdl);
  oHdr.className = sreq.assocClass;

  binCtx.oHdr = (OperationHdr *) & oHdr;
  binCtx.bHdr = &sreq.hdr;
  binCtx.bHdr->flags = 0;
  binCtx.rHdr = NULL;
  binCtx.bHdrSize = sizeof(sreq);
  binCtx.type = CMPI_ref;
  binCtx.chunkedMode = binCtx.xmlAs = binCtx.noResp = 0;
  binCtx.pAs = NULL;

  _SFCB_TRACE(1, ("--- Getting Provider context"));
  irc = getProviderContext(&binCtx);

  CMRelease(ns);

  if (irc == MSG_X_PROVIDER) {
    _SFCB_TRACE(1, ("--- Calling Providers"));
    resp = invokeProviders(&binCtx, &err, &l);

    closeSockets(&binCtx);
    closeProviderContext(&binCtx);

    if (err == 0) {
      enm = cpyEnumResponses(&binCtx, resp, l);
      freeResps(resp, binCtx.pCount);
      _SFCB_RETURN(enm);
    }
    if (rc)
      CIMCSetStatusWithChars(rc, resp[err - 1]->rc,
                             (char *) resp[err - 1]->object[0].data);
    freeResps(resp, binCtx.pCount);
    _SFCB_RETURN(NULL);
  } else
    ctxErrResponse(&binCtx, rc);
  closeProviderContext(&binCtx);

  _SFCB_RETURN(NULL);
}

static CMPIEnumeration *
referenceNames(Client * mb,
               CMPIObjectPath * cop,
               const char *resultClass, const char *role, CMPIStatus *rc)
{
  ReferenceNamesReq sreq = BINREQ(OPS_ReferenceNames, RIN_REQ_REG_SEGMENTS);
  int             irc,
                  l = 0,
      err = 0;
  BinResponseHdr **resp;
  BinRequestContext binCtx;
  OperationHdr    oHdr = { OPS_ReferenceNames, 0, 4 };
  CMPIEnumeration *enm = NULL;

  _SFCB_ENTER(TRACE_CIMXMLPROC, "referenceNames");

  if (rc)
    CMSetStatus(rc, CMPI_RC_OK);

  CMPIString     *ns = cop->ft->getNameSpace(cop, NULL);

  memset(&binCtx, 0, sizeof(BinRequestContext));

  sreq.objectPath = setObjectPathMsgSegment(cop);
  sreq.resultClass = setCharsMsgSegment(resultClass);
  sreq.role = setCharsMsgSegment(role);
  sreq.principal = setCharsMsgSegment(((ClientEnc *) mb)->data.user);
  sreq.userRole = setCharsMsgSegment(NULL);

  oHdr.className = sreq.resultClass;
  oHdr.nameSpace = setCharsMsgSegment((char *) ns->hdl);

  binCtx.oHdr = (OperationHdr *) & oHdr;
  binCtx.bHdr = &sreq.hdr;
  binCtx.bHdr->flags = 0;
  binCtx.rHdr = NULL;
  binCtx.bHdrSize = sizeof(sreq);
  binCtx.type = CMPI_ref;
  binCtx.chunkedMode = binCtx.xmlAs = binCtx.noResp = 0;
  binCtx.pAs = NULL;

  _SFCB_TRACE(1, ("--- Getting Provider context"));
  irc = getProviderContext(&binCtx);

  CMRelease(ns);

  if (irc == MSG_X_PROVIDER) {
    _SFCB_TRACE(1, ("--- Calling Providers"));
    resp = invokeProviders(&binCtx, &err, &l);

    closeSockets(&binCtx);
    closeProviderContext(&binCtx);

    if (err == 0) {
      enm = cpyEnumResponses(&binCtx, resp, l);
      freeResps(resp, binCtx.pCount);
      _SFCB_RETURN(enm);
    }
    if (rc)
      CIMCSetStatusWithChars(rc, resp[err - 1]->rc,
                             (char *) resp[err - 1]->object[0].data);
    freeResps(resp, binCtx.pCount);
    _SFCB_RETURN(NULL);
  } else
    ctxErrResponse(&binCtx, rc);
  closeProviderContext(&binCtx);

  _SFCB_RETURN(NULL);
}

#ifdef SFCB_DEBUG
static CMPIValue convertFromStringValue(CMPIValue strcval, 
                                        CMPIType strctype, 
                                        CMPIType xtype,
                                        CMPIStatus *prc)
{
  char *sval = NULL;
  char *scheck  = NULL;
  CMPIValue newcval = {CMPI_null};
  
  _SFCB_ENTER(TRACE_CIMXMLPROC, "convertFromStringValue");
  
  // expected incoming type is either string or string array
  // incoming array type must match expected array type 
  if (!(xtype & CMPI_ARRAY) && (strctype & CMPI_ARRAY)) {
    if (prc) CMSetStatus(prc, CMPI_RC_ERR_INVALID_PARAMETER);
    return newcval;
  }
  
  if (CMPI_chars == strctype) {
    sval = strcval.chars;
  } else if (CMPI_string == strctype) {
    sval = (char *)CMGetCharsPtr(strcval.string, NULL);
  }

  // convert to expected type
  switch (xtype) {
  case CMPI_string:
    newcval.string = sfcb_native_new_CMPIString(sval, NULL,0);
    break;
  case CMPI_char16:
    newcval.char16 = *sval;
    break;
  case CMPI_dateTime:
    newcval.dateTime = NewCMPIDateTimeFromChars(sval, NULL);
    break;
  case CMPI_real64:
    newcval.real64 = (CMPIReal64) strtod(sval, &scheck);
    break;
  case CMPI_sint64:
    newcval.sint64 = strtoll(sval, &scheck, 0); 
    break;
  case CMPI_uint64:
    newcval.uint64 = strtoull(sval, &scheck, 0); 
    break;
  case CMPI_real32:
    newcval.real32 = (CMPIReal32) strtod(sval, &scheck);
    break;
  case CMPI_sint32:
    newcval.sint32 = strtol(sval, &scheck, 0);
    break;
  case CMPI_uint32:
    newcval.uint32 = strtoul(sval, &scheck, 0);
    break;
  case CMPI_sint16:
    newcval.sint16 = (CMPISint16) strtol(sval, &scheck, 0);
    break;
  case CMPI_uint16:
    newcval.uint16 = (CMPIUint16) strtoul(sval, &scheck, 0);
    break;
  case CMPI_uint8:
    newcval.uint8 = (CMPIUint8) strtoul(sval, &scheck, 0);
    break;
  case CMPI_sint8:
    newcval.sint8 = (CMPISint8) strtol(sval, &scheck, 0);
    break;
  case CMPI_boolean:
    if ((strcasecmp(sval, "false") == 0))  {
      newcval.boolean = 0; 
    } else if ((strcasecmp(sval, "true") == 0)) {
      newcval.boolean = 1;
    } else {
      scheck = sval; // set failure case
    }
    break;
  default:
    if (xtype & CMPI_ARRAY) {
      if (!(strctype & CMPI_ARRAY)) {
        // incoming is not array -> convert to array
        newcval.array = NewCMPIArray(1, (xtype ^ CMPI_ARRAY), NULL);
        CMPIValue arrayvalue = convertFromStringValue(strcval, strctype, (xtype ^ CMPI_ARRAY), prc);
        if (prc && (prc->rc == CMPI_RC_OK)) {
          CMSetArrayElementAt(newcval.array, 0, &arrayvalue, (xtype ^ CMPI_ARRAY));
        } else {
          // conversion failed
          CMRelease(newcval.array);
          scheck = sval; // set failure case
        }                       
      } else {
        // convert to and from arrays
        int ii, arcount = CMGetArrayCount(strcval.array, NULL);
        newcval.array = NewCMPIArray(arcount, (xtype ^ CMPI_ARRAY), NULL);
        for (ii = 0; ii < arcount; ii++) {
          CMPIValue arvalue;
          CMPIData data = CMGetArrayElementAt(strcval.array, ii, NULL);
          arvalue = convertFromStringValue(data.value, (strctype ^ CMPI_ARRAY), (xtype ^ CMPI_ARRAY), prc);
          if (prc && (prc->rc == CMPI_RC_OK)) {
            CMSetArrayElementAt(newcval.array, ii, &arvalue, (xtype ^ CMPI_ARRAY));
          } else {
            // conversion failed
            CMRelease(newcval.array);
            scheck = sval; // set failure case
            break;
          }            
        }
      } 
    } else {
      _SFCB_TRACE(1,("--- cannot convert value: %s to type: %u",sval,xtype));
      if (prc) CMSetStatus(prc, CMPI_RC_ERR_INVALID_PARAMETER);  
    }
  } // switch

  // detect if conversion failed
  if (scheck && *scheck) {
    _SFCB_TRACE(1,("--- failed conversion of value: %s to type: %u",sval,xtype));
    if (prc) CMSetStatus(prc, CMPI_RC_ERR_INVALID_PARAMETER);
  }

  _SFCB_RETURN(newcval);
} // convertFromStringValue
#endif

static CMPIArgs * convertFromStringArguments(CMPIConstClass *cls, 
                                             const char *method, 
                                             CMPIArgs *argsin, 
                                             CMPIStatus *prc)
{
  CMPIArgs *argsnew;
  ClClass *cl;
  ClMethod *meth;
  char *mname;
  int mm, ii, jj, incount, vmpt = 0;

  _SFCB_ENTER(TRACE_CIMXMLPROC, "convertFromStringArguments");

  if (!cls) {
    _SFCB_TRACE(1,("--- class not found"));
    goto error1;
  }
  cl = (ClClass *)cls->hdl;

  // check that the method specified in the request exist in the class definition
  _SFCB_TRACE(4,("--- class method count: %u", ClClassGetMethodCount(cl)));
  for (jj = 0, mm = ClClassGetMethodCount(cl); jj < mm; jj++) {
    ClClassGetMethodAt(cl, jj, NULL, &mname, NULL);
    _SFCB_TRACE(4,("--- index: %u name: %s", jj, mname));
    if (strcasecmp(method, mname) == 0) {
      break;
    }
  }
  if (jj == mm) {
    _SFCB_TRACE(1,("--- failed to find matching method: %s in class", method));
    goto error1;
  }
  meth = ((ClMethod*)ClObjectGetClSection(&cl->hdr, &cl->methods)) + jj;

  argsnew = NewCMPIArgs(NULL);
  if (getControlBool("validateMethodParamTypes", &vmpt)) {
    vmpt=1;
  }

  // loop through the method parameters in the request
  incount = CMGetArgCount(argsin, NULL);
  for (ii = 0; ii < incount; ii++) {
    CMPIString *name;
    char *sname;
    CMPIData cdata = CMGetArgAt(argsin,ii,&name,NULL);
    CMPIParameter cparam;
    int pp, pm;

    _SFCB_TRACE(4,("--- index: %u name: %s type: %u",ii,(char*)name->hdl,cdata.type)); 
    // loop through the method parameters defined in the class
    for (pp = 0, pm = ClClassGetMethParameterCount(cl, jj); pp < pm; pp++) {
      ClClassGetMethParameterAt(cl, meth, pp, &cparam, &sname);
      _SFCB_TRACE(4,("--- class cparam: %s",sname)); 
      if (strcasecmp(sname, (char*)name->hdl) == 0) {
        _SFCB_TRACE(4,("--- match found: %s at index: %u",(char*)name->hdl,pp)); 
        break;
      }
    }
    if (pp == pm) {
      _SFCB_TRACE(1,("--- parameter: %s not found\n",(char*)name->hdl));
      if (0 != vmpt) {
        goto error2;
      }
      goto addacopy;
    }
    _SFCB_TRACE(4,("--- name: %s datatype: %u registeredtype: %u",(char*)name->hdl,cdata.type,cparam.type)); 
    if (cdata.type == 0) {
      _SFCB_TRACE(1,("--- type is 0 so data value could be anything, not converting"));
      if (0 != vmpt) {
        goto error2;
      }
      goto addacopy;
    }
    if (cdata.type != cparam.type) {
      // request parameter type does not match corresponding class definition
      if ((cdata.type & CMPI_ref) == CMPI_ref) {
        // incoming type is reference
        if (!(cdata.type & CMPI_ARRAY) && (cparam.type & CMPI_ARRAY)) {
          // incoming is not array, class definition is array -> change to array type
          CMPIValue refcval;
          refcval.array = NewCMPIArray(1, (cparam.type ^ CMPI_ARRAY), NULL);
          CMSetArrayElementAt(refcval.array, 0, &cdata.value, (cparam.type ^ CMPI_ARRAY));
          CMAddArg(argsnew, (char*)name->hdl, &refcval, cparam.type);
          continue;
        }
        else if((cdata.type & CMPI_ARRAY) && !(cparam.type & CMPI_ARRAY)) {
          // incoming is array, class definition is not array -> ERROR
          if(prc) CMSetStatus(prc, CMPI_RC_ERR_INVALID_PARAMETER);
          goto error2;
        }
        // type already matched
        goto addacopy;    
      } else {
        // incoming type must be string
        if (!(cdata.type & CMPI_chars) && !(cdata.type & CMPI_string)) {
          _SFCB_TRACE(1,("--- type mismatch but user type is not string, not converting")); 
          if (0 != vmpt) {
            goto error2;
          }
          goto addacopy;
        }
        // convert the string value to defined type
        _SFCB_TRACE_VAR(CMPIValue newcval = convertFromStringValue(cdata.value, cdata.type, cparam.type, prc));
        if (prc && (prc->rc != CMPI_RC_OK)) {
          if (0 != vmpt) {
            goto error2;
          }
          // clear the rc value since we are ignoring the conversion failure
          prc->rc = CMPI_RC_OK;
          goto addacopy;
        }
        _SFCB_TRACE_VAR(CMPIStatus lrc = CMAddArg(argsnew, (char*)name->hdl, &newcval, cparam.type));
        _SFCB_TRACE(4,("--- IN argument: %s updated with ptype: %u status: %u",(char*)name->hdl,cparam.type,lrc.rc)); 
        continue;
      }
    }
  addacopy:
    CMAddArg(argsnew, (char*)name->hdl, &cdata.value, cdata.type);
  } // for

  _SFCB_RETURN(argsnew);

 error2:
  CMRelease(argsnew);
 error1:
  _SFCB_TRACE(1,("--- conversion failed, returning a copy of the source")); 
  _SFCB_RETURN(argsin);
} // convertFromStringArguments


static CMPIData
invokeMethod(Client * mb,
             CMPIObjectPath * cop,
             const char *method,
             CMPIArgs * in, CMPIArgs * out, CMPIStatus *rc)
{
  InvokeMethodReq sreq = BINREQ(OPS_InvokeMethod, 5);
  int             irc,
                  i,
                  outc;
  BinResponseHdr *resp;
  BinRequestContext binCtx;
  OperationHdr    oHdr = { OPS_InvokeMethod, 0, 5 };
  CMPIArgs       *argsin = NULL;
  CMPIArgs       *argsout;
  CMPIData        retval = { 0, CMPI_notFound, {0l} };
  if (rc)
    CMSetStatus(rc, CMPI_RC_OK);

  _SFCB_ENTER(TRACE_CIMXMLPROC, "invokeMethod");

  CMPIString     *ns = cop->ft->getNameSpace(cop, NULL);
  CMPIString     *cn = cop->ft->getClassName(cop, NULL);

  oHdr.nameSpace = setCharsMsgSegment((char *) ns->hdl);
  oHdr.className = setCharsMsgSegment((char *) cn->hdl);

  memset(&binCtx, 0, sizeof(BinRequestContext));

  CMPIConstClass *tcls = getClass(mb, cop, 0, NULL, rc);
  argsin = convertFromStringArguments(tcls, method, in, rc);
  CMRelease(tcls);
  tcls = NULL;

  if(rc && rc->rc == CMPI_RC_OK) {
    sreq.objectPath = setObjectPathMsgSegment(cop);
    sreq.principal = setCharsMsgSegment(((ClientEnc *) mb)->data.user);
    sreq.in = setArgsMsgSegment(argsin);
    sreq.out = setArgsMsgSegment(NULL);
    sreq.method = setCharsMsgSegment(method);

    binCtx.oHdr = (OperationHdr *) & oHdr;
    binCtx.bHdr = &sreq.hdr;
    binCtx.rHdr = NULL;
    binCtx.bHdrSize = sizeof(sreq);
    binCtx.chunkedMode = binCtx.xmlAs = binCtx.noResp = 0;
    binCtx.pAs = NULL;

    _SFCB_TRACE(1, ("--- Getting Provider context"));
    irc = getProviderContext(&binCtx);

    CMRelease(ns);
    CMRelease(cn);

    if (irc == MSG_X_PROVIDER) {
      _SFCB_TRACE(1, ("--- Calling Provider"));
      resp = invokeProvider(&binCtx);

      closeSockets(&binCtx);
      closeProviderContext(&binCtx);
      if (argsin != in)
        CMRelease(argsin);

      resp->rc--;
      if (resp->rc == CMPI_RC_OK) {
        argsout = relocateSerializedArgs(resp->object[0].data);
        outc = CMGetArgCount(argsout, NULL);
        for (i = 0; i < outc; i++) {
          CMPIString     *name;
          CMPIData        data = CMGetArgAt(argsout, i, &name, NULL);
          CMAddArg(out, (char *) name->hdl, &data.value, data.type);
        }
        if (resp->rvValue) {
          /*
           * check method return value for pass-by-reference types 
           */
          if (resp->rv.type == CMPI_chars) {
            resp->rv.value.chars =
              strdup((long) resp->rvEnc.data + (char *) resp);
          } else if (resp->rv.type == CMPI_dateTime) {
            resp->rv.value.dateTime =
              NewCMPIDateTimeFromChars((long) resp->rvEnc.data
                                       + (char *) resp, NULL);
          }
        }
        retval = resp->rv;
        free(resp);
        _SFCB_RETURN(retval);
      }
      if (rc)
        CIMCSetStatusWithChars(rc, resp->rc, (char *) resp->object[0].data);
      free(resp);
      _SFCB_RETURN(retval);
    } else
      ctxErrResponse(&binCtx, rc);
    closeProviderContext(&binCtx);
  }

  if (argsin != in)
    CMRelease(argsin);

  _SFCB_RETURN(retval);
}

static CMPIStatus
setProperty(Client __attribute__ ((unused)) *mb,
            CMPIObjectPath __attribute__ ((unused)) *cop,
            const char __attribute__ ((unused)) *name, 
            CMPIValue __attribute__ ((unused)) *value, 
            CMPIType __attribute__ ((unused)) type)
{
  CMPIStatus      rc;
  CMSetStatus(&rc, CMPI_RC_ERR_NOT_SUPPORTED);

  return rc;
}

static CMPIData
getProperty(Client __attribute__ ((unused)) *mb,
            CMPIObjectPath __attribute__ ((unused)) *cop, 
            const char __attribute__ ((unused)) *name, 
            CMPIStatus __attribute__ ((unused)) *rc)
{
  CMPIData        retval = { 0, CMPI_notFound, {0l} };
  if (rc)
    CMSetStatus(rc, CMPI_RC_ERR_NOT_SUPPORTED);
  return retval;
}

/*
 * --------------------------------------------------------------------------
 */
static CMPIConstClass *
getClass(Client * mb,
         CMPIObjectPath * cop,
         CMPIFlags flags, char **properties, CMPIStatus *rc)
{
  CMPIConstClass *cls;
  int             irc,
                  i,
                  sreqSize = sizeof(GetClassReq) - sizeof(MsgSegment);
  BinRequestContext binCtx;
  BinResponseHdr *resp;
  GetClassReq    *sreq;
  int             pCount = 0;
  OperationHdr    oHdr = { OPS_GetClass, 0, 2 };

  _SFCB_ENTER(TRACE_CIMXMLPROC, "getClass");

  if (rc)
    CMSetStatus(rc, CMPI_RC_OK);

  CMPIString     *ns = cop->ft->getNameSpace(cop, NULL);
  CMPIString     *cn = cop->ft->getClassName(cop, NULL);

  oHdr.nameSpace = setCharsMsgSegment((char *) ns->hdl);
  oHdr.className = setCharsMsgSegment((char *) cn->hdl);

  memset(&binCtx, 0, sizeof(BinRequestContext));

  if (properties) {
    char          **p;
    for (p = properties; *p; p++)
      pCount++;
  }

  sreqSize += pCount * sizeof(MsgSegment);
  sreq = calloc(1, sreqSize);
  sreq->hdr.operation = OPS_GetClass;
  sreq->hdr.count = pCount + 2;

  sreq->objectPath = setObjectPathMsgSegment(cop);
  sreq->principal = setCharsMsgSegment(((ClientEnc *) mb)->data.user);

  for (i = 0; i < pCount; i++)
    sreq->properties[i] = setCharsMsgSegment(properties[i]);

  binCtx.oHdr = (OperationHdr *) & oHdr;
  binCtx.bHdr = &sreq->hdr;
  binCtx.bHdr->flags = flags;
  binCtx.rHdr = NULL;
  binCtx.bHdrSize = sreqSize;
  binCtx.chunkedMode = binCtx.xmlAs = binCtx.noResp = 0;
  binCtx.pAs = NULL;

  _SFCB_TRACE(1, ("--- Getting Provider context"));
  irc = getProviderContext(&binCtx);

  CMRelease(ns);
  CMRelease(cn);

  _SFCB_TRACE(1, ("--- Provider context gotten"));
  if (irc == MSG_X_PROVIDER) {
    resp = invokeProvider(&binCtx);

    closeSockets(&binCtx);
    closeProviderContext(&binCtx);

    resp->rc--;
    if (resp->rc == CMPI_RC_OK) {
      cls = relocateSerializedConstClass(resp->object[0].data);
      cls = cls->ft->clone(cls, NULL);
      free(resp);
      free(sreq);
      _SFCB_RETURN(cls);
    }
    free(sreq);
    if (rc)
      CIMCSetStatusWithChars(rc, resp->rc, (char *) resp->object[0].data);
    free(resp);
    _SFCB_RETURN(NULL);
  } else
    ctxErrResponse(&binCtx, rc);
  free(sreq);
  closeProviderContext(&binCtx);

  _SFCB_RETURN(NULL);
}

static CMPIEnumeration *
enumClassNames(Client * mb,
               CMPIObjectPath * cop, CMPIFlags flags, CMPIStatus *rc)
{
  EnumClassNamesReq sreq = BINREQ(OPS_EnumerateClassNames, 2);
  int             irc,
                  l = 0,
      err = 0;
  BinResponseHdr **resp;
  BinRequestContext binCtx;
  OperationHdr    oHdr = { OPS_EnumerateClassNames, 0, 2 };
  CMPIEnumeration *enm = NULL;

  _SFCB_ENTER(TRACE_CIMXMLPROC, "enumClassNames");

  if (rc)
    CMSetStatus(rc, CMPI_RC_OK);

  CMPIString     *ns = cop->ft->getNameSpace(cop, NULL);
  CMPIString     *cn = cop->ft->getClassName(cop, NULL);

  oHdr.nameSpace = setCharsMsgSegment((char *) ns->hdl);
  oHdr.className = setCharsMsgSegment((char *) cn->hdl);

  memset(&binCtx, 0, sizeof(BinRequestContext));

  sreq.objectPath = setObjectPathMsgSegment(cop);
  sreq.principal = setCharsMsgSegment(((ClientEnc *) mb)->data.user);
  sreq.hdr.flags = flags;

  binCtx.oHdr = (OperationHdr *) & oHdr;
  binCtx.bHdr = &sreq.hdr;
  binCtx.bHdr->flags = flags;
  binCtx.rHdr = NULL;
  binCtx.bHdrSize = sizeof(sreq);
  binCtx.type = CMPI_ref;
  binCtx.chunkedMode = binCtx.xmlAs = binCtx.noResp = 0;
  binCtx.pAs = NULL;

  _SFCB_TRACE(1, ("--- Getting Provider context"));
  irc = getProviderContext(&binCtx);

  CMRelease(ns);
  CMRelease(cn);

  if (irc == MSG_X_PROVIDER) {
    _SFCB_TRACE(1, ("--- Calling Providers"));
    resp = invokeProviders(&binCtx, &err, &l);
    _SFCB_TRACE(1, ("--- Back from Provider"));

    closeSockets(&binCtx);
    closeProviderContext(&binCtx);

    if (err == 0) {
      enm = cpyEnumResponses(&binCtx, resp, l);
      freeResps(resp, binCtx.pCount);
      _SFCB_RETURN(enm);
    }
    if (rc)
      CIMCSetStatusWithChars(rc, resp[err - 1]->rc,
                             (char *) resp[err - 1]->object[0].data);
    freeResps(resp, binCtx.pCount);
    _SFCB_RETURN(NULL);
  } else
    ctxErrResponse(&binCtx, rc);
  closeProviderContext(&binCtx);

  _SFCB_RETURN(NULL);
}

static CMPIEnumeration *
enumClasses(Client * mb,
            CMPIObjectPath * cop, CMPIFlags flags, CMPIStatus *rc)
{
  EnumClassesReq  sreq = BINREQ(OPS_EnumerateClasses, 2);
  int             irc,
                  l = 0,
      err = 0;
  BinResponseHdr **resp;
  BinRequestContext binCtx;
  OperationHdr    oHdr = { OPS_EnumerateClasses, 0, 2 };
  CMPIEnumeration *enm = NULL;

  _SFCB_ENTER(TRACE_CIMXMLPROC, "enumClasses");

  if (rc)
    CMSetStatus(rc, CMPI_RC_OK);

  CMPIString     *ns = cop->ft->getNameSpace(cop, NULL);
  CMPIString     *cn = cop->ft->getClassName(cop, NULL);

  oHdr.nameSpace = setCharsMsgSegment((char *) ns->hdl);
  oHdr.className = setCharsMsgSegment((char *) cn->hdl);

  memset(&binCtx, 0, sizeof(BinRequestContext));

  sreq.objectPath = setObjectPathMsgSegment(cop);
  sreq.principal = setCharsMsgSegment(((ClientEnc *) mb)->data.user);
  sreq.hdr.flags = flags;

  binCtx.oHdr = (OperationHdr *) & oHdr;
  binCtx.bHdr = &sreq.hdr;
  binCtx.bHdr->flags = flags;
  binCtx.type = CMPI_class;
  binCtx.rHdr = NULL;
  binCtx.bHdrSize = sizeof(sreq);
  binCtx.chunkedMode = binCtx.xmlAs = binCtx.noResp = 0;
  binCtx.pAs = NULL;

  _SFCB_TRACE(1, ("--- Getting Provider context"));
  irc = getProviderContext(&binCtx);

  CMRelease(ns);
  CMRelease(cn);

  if (irc == MSG_X_PROVIDER) {
    _SFCB_TRACE(1, ("--- Calling Providers"));
    resp = invokeProviders(&binCtx, &err, &l);

    closeSockets(&binCtx);
    closeProviderContext(&binCtx);

    if (err == 0) {
      enm = cpyEnumResponses(&binCtx, resp, l);
      freeResps(resp, binCtx.pCount);
      _SFCB_RETURN(enm);
    }
    if (rc)
      CIMCSetStatusWithChars(rc, resp[err - 1]->rc,
                             (char *) resp[err - 1]->object[0].data);
    freeResps(resp, binCtx.pCount);
    _SFCB_RETURN(NULL);
  } else
    ctxErrResponse(&binCtx, rc);
  closeProviderContext(&binCtx);

  _SFCB_RETURN(NULL);

}

static ClientFT clientFt = {
  1,                            // NATIVE_FT_VERSION,
  releaseClient,
  cloneClient,
  getClass,
  enumClassNames,
  enumClasses,
  getInstance,
  createInstance,
  modifyInstance,
  deleteInstance,
  execQuery,
  enumInstanceNames,
  enumInstances,
  associators,
  associatorNames,
  references,
  referenceNames,
  invokeMethod,
  setProperty,
  getProperty
};

/*
 * --------------------------------------------------------------------------
 */

extern int      getControlChars(char *, char **);
extern int      setupControl(char *fn);
extern int      errno;

static int      localConnectCount = 0;
static pthread_mutex_t lcc_mutex = PTHREAD_MUTEX_INITIALIZER;

#define CONNECT_LOCK()   pthread_mutex_lock(&lcc_mutex)
#define CONNECT_UNLOCK() pthread_mutex_unlock(&lcc_mutex)
#define CONNECT_UNLOCK_RETURN(rc) { pthread_mutex_unlock(&lcc_mutex); return (rc);}

int
localConnect(ClientEnv *ce, CMPIStatus *st)
{
  static struct sockaddr_un serverAddr = {AF_UNIX, {'\0'}};
  int             sock,
                  rc = 0,
      sfcbSocket;
  void           *idData;
  unsigned long int l;
  char           *user;
  static char    *socketName = NULL;

  struct _msg {
    unsigned int    size;
    char            oper;
    pid_t           pid;
    char            id[64];
  } msg;

  CONNECT_LOCK();
  if (localConnectCount == 0) {
    if ((sock = socket(PF_UNIX, SOCK_STREAM, 0)) < 0) {
      if (st) {
        st->rc = CMPI_RC_ERR_FAILED;
        st->msg = ce->ft->newString(ce, strerror(errno), NULL);
      }
      CONNECT_UNLOCK_RETURN(-1);
    }

    if (socketName == NULL) {
      setupControl(NULL);
      rc = getControlChars("localSocketPath", &socketName);
      if (rc == 0) {
        strcpy(serverAddr.sun_path, socketName);
        sunsetControl();
      }
      else {
        if (st) {
          st->rc = CMPI_RC_ERR_FAILED;
          st->msg =
              ce->ft->newString(ce, "failed to open sfcb local socket",
                                NULL);
        }
        fprintf(stderr, "--- Failed to open sfcb local socket (%d)\n", rc);
        sunsetControl();
        close(sock);
        CONNECT_UNLOCK_RETURN(-2);
      }
    }

    if (serverAddr.sun_path[0] == '\0')
      strcpy(serverAddr.sun_path, socketName);

    if (connect(sock, (struct sockaddr *) &serverAddr,
                sizeof(serverAddr.sun_family) +
                strlen(serverAddr.sun_path)) < 0) {
      if (st) {
        st->rc = CMPI_RC_ERR_FAILED;
        st->msg = ce->ft->newString(ce, strerror(errno), NULL);
      }
      close(sock);
      CONNECT_UNLOCK_RETURN(-1);
    }

    msg.size = sizeof(msg) - sizeof(msg.size);
    msg.oper = 1;
    msg.pid = getpid();
    user = getenv("USER");
    strncpy(msg.id, (user) ? user : "", sizeof(msg.id) - 1);
    msg.id[sizeof(msg.id) - 1] = 0;

    l = write(sock, &msg, sizeof(msg));

    rc = spRecvCtlResult(&sock, &sfcbSocket, &idData, &l);
    if (rc < 0 || sfcbSocket <= 0) {
      if (st) {
        st->rc = CMPI_RC_ERR_FAILED;
        st->msg =
            ce->ft->newString(ce,
                              "failed to get socket fd for local connect",
                              NULL);
      }
      fprintf(stderr,
              "--- Failed to get socket fd for local connect (%d, %d, %lu)\n",
              rc, sfcbSocket, l);
      close(sock);
      CONNECT_UNLOCK_RETURN(-3);
    }
    sfcbSockets.send = sfcbSocket;

    close(sock);
  }
  localConnectCount += 1;
  CONNECT_UNLOCK();
  localMode = 0;

  return (rc == 0) ? rc : sfcbSocket;
}

/* ReleaseCIMCEnv() in SFCC will take care of the dlclose() */
static void    *
release(ClientEnv *ce)
{
  closeLogging(0);
  CONNECT_LOCK();
  if (localConnectCount > 0)
    localConnectCount -= 1;
  if (localConnectCount == 0) {
    close(sfcbSockets.send);
    sfcbSockets.send = -1;
  }
  CONNECT_UNLOCK();
  sunsetControl();
  uninitGarbageCollector();
  return NULL;
}

static Client  *CMPIConnect2(ClientEnv *ce, const char *hn,
                             const char *scheme, const char *port,
                             const char *user, const char *pwd,
                             int verifyMode, const char *trustStore,
                             const char *certFile, const char *keyFile,
                             CMPIStatus *rc);

static Client  *
CMPIConnect(ClientEnv *ce, const char *hn, const char *scheme,
            const char *port, const char *user, const char *pwd,
            CMPIStatus *rc)
{
  return CMPIConnect2(ce, hn, scheme, port, user, pwd, Client_VERIFY_PEER,
                      NULL, NULL, NULL, rc);
}

static Client  *
CMPIConnect2(ClientEnv *ce, const char *hn, const char *scheme,
             const char *port, const char *user, const char *pwd,
             int verifyMode, const char *trustStore, const char *certFile,
             const char *keyFile, CMPIStatus *rc)
{
  ClientEnc      *cc;

  if (rc)
    CMSetStatus(rc, 0);

  if (localConnect(ce, rc) < 0)
    return NULL;

  cc = calloc(1, sizeof(*cc));

  cc->enc.hdl = &cc->data;
  cc->enc.ft = &clientFt;

  cc->data.hostName = hn ? strdup(hn) : strdup("localhost");
  cc->data.user = user ? strdup(user) : NULL;
  cc->data.pwd = pwd ? strdup(pwd) : NULL;
  cc->data.scheme = scheme ? strdup(scheme) : strdup("http");

  if (port != NULL)
    cc->data.port = strdup(port);
  else
    cc->data.port = strcmp(cc->data.scheme, "https") == 0 ?
        strdup("5989") : strdup("5988");

  cc->certData.verifyMode = verifyMode;
  cc->certData.trustStore = trustStore ? strdup(trustStore) : NULL;
  cc->certData.certFile = certFile ? strdup(certFile) : NULL;
  cc->certData.keyFile = keyFile ? strdup(keyFile) : NULL;

  char *lcto_env = getenv("SFCC_LOCALCONNECT_CLIENT_TIMEOUT");
  if (lcto_env) {
    long lcto = (long) atoi(lcto_env);
    if (lcto > 0) {
      /* Of course we are not using http here, but this is
       * a simple way to make spGetMsg() obey the timeout */
      httpReqHandlerTimeout = (lcto < 5) ? 5 : lcto;
      httpProcIdX = 1;
    }
  }
  return (Client *) cc;
}

static CMPIInstance *
newInstance(ClientEnv __attribute__ ((unused)) *ce, const CMPIObjectPath * op, CMPIStatus *rc)
{
  return NewCMPIInstance((CMPIObjectPath *) op, rc);
}

static CMPIString *
newString(ClientEnv __attribute__ ((unused)) *ce, const char *ptr, CMPIStatus *rc)
{
  return NewCMPIString(ptr, rc);
}

static CMPIObjectPath *
newObjectPath(ClientEnv __attribute__ ((unused)) *ce, const char *ns, const char *cn,
              CMPIStatus *rc)
{
  return NewCMPIObjectPath(ns, cn, rc);
}

static CMPIArgs *
newArgs(ClientEnv __attribute__ ((unused)) *ce, CMPIStatus *rc)
{
  return NewCMPIArgs(rc);
}

static CMPIArray *
newArray(ClientEnv __attribute__ ((unused)) *ce, CMPICount max, CMPIType type, CMPIStatus *rc)
{
  return NewCMPIArray(max, type, rc);
}

static CMPIDateTime *
newDateTime(ClientEnv __attribute__ ((unused)) *ce, CMPIStatus *rc)
{
  return NewCMPIDateTime(rc);
}

static CMPIDateTime *
newDateTimeFromBinary(ClientEnv __attribute__ ((unused)) *ce, CMPIUint64 binTime,
                      CMPIBoolean interval, CMPIStatus *rc)
{
  return NewCMPIDateTimeFromBinary(binTime, interval, rc);
}

static CMPIDateTime *
newDateTimeFromChars(ClientEnv __attribute__ ((unused)) *ce, const char *utcTime, CMPIStatus *rc)
{
  return NewCMPIDateTimeFromChars(utcTime, rc);
}

void _Cleanup_SfcbLocal_Env(void)
{
    _SFCB_TRACE_STOP();
}

void *
newIndicationListener(ClientEnv *ce, int sslMode, int *portNumber, char **socketName,
                     void (*fp) (CMPIInstance *indInstance), CMPIStatus* rc)
{
  fprintf(stderr, "*** newIndicationListener not supported for SfcbLocal\n");
  return NULL;
}

void * _markHeap() {
  return markHeap();
}

void _releaseHeap(void* heap) {
  releaseHeap(heap);
  return;
}

/* called by SFCC's NewCIMCEnv() */
ClientEnv      *
_Create_SfcbLocal_Env(char __attribute__ ((unused)) *id, unsigned int options, 
                      int __attribute__ ((unused)) *rc, char __attribute__ ((unused)) **msg)
{

  static ClientEnvFT localFT = {
    "SfcbLocal",
    release,
    CMPIConnect,
    CMPIConnect2,
    newInstance,
    newObjectPath,
    newArgs,
    newString,
    newArray,
    newDateTime,
    newDateTimeFromBinary,
    newDateTimeFromChars,
    newIndicationListener,
    _markHeap,
    _releaseHeap
  };

  // localClientMode=1;
  setInstanceLocalMode(1);

  ClientEnv      *env = malloc(sizeof(*env));
  env->hdl = NULL;
  env->ft = &localFT;
  env->options = options;

  // enable trace logging
  _SFCB_TRACE_INIT();
  {
    char *var;
    int tracemask = 0, tracelevel = 0;
    if (NULL != (var = getenv("SFCB_TRACE"))) {
      tracelevel = atoi(var);
    }
    if (NULL != (var = getenv("SFCB_TRACE_MASK"))) {
      tracemask = atoi(var);
    }
    _SFCB_TRACE_START(tracelevel, tracemask);
  }
  atexit(_Cleanup_SfcbLocal_Env);

  return env;
}

/* MODELINES */
/* DO NOT EDIT BELOW THIS COMMENT */
/* Modelines are added by 'make pretty' */
/* -*- Mode: C; c-basic-offset: 2; indent-tabs-mode: nil; -*- */
/* vi:set ts=2 sts=2 sw=2 expandtab: */
