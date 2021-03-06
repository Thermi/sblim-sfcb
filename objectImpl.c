
/*
 * objectimpl.c
 *
 * (C) Copyright IBM Corp. 2005
 *
 * THIS FILE IS PROVIDED UNDER THE TERMS OF THE ECLIPSE PUBLIC LICENSE
 * ("AGREEMENT"). ANY USE, REPRODUCTION OR DISTRIBUTION OF THIS FILE
 * CONSTITUTES RECIPIENTS ACCEPTANCE OF THE AGREEMENT.
 *
 * You can obtain a current copy of the Eclipse Public License from
 * http://www.opensource.org/licenses/eclipse-1.0.php
 *
 * Author:     Adrian Schuur <schuur@de.ibm.com>
 *
 * Description:
 *
 * Internal implementation support for cim objects.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <netinet/in.h>

#include "objectImpl.h"
#include "array.h"
#include <sfcCommon/utilft.h>
#include "trace.h"
#include "config.h"

#ifdef SFCB_IX86
#define SFCB_ASM(x) asm(x)
#else
#define SFCB_ASM(x)
#endif

// #define DEB(x) x
#define DEB(x)

// static ClSection *newClSection();

static ClClass *newClassH(ClObjectHdr * hdr, const char *cn,
                          const char *pa);
static ClClass *rebuildClassH(ClObjectHdr * hdr, ClClass * cls,
                              void *area);

static char    *addQualifierToString(stringControl * sc, ClObjectHdr * hdr,
                                     ClQualifier * q, int sb);
static int      addClQualifier(ClObjectHdr * hdr, ClSection * qlfs,
                               const char *id, CMPIData d,
                               ClObjectHdr * arrHdr);
			       
static long addClStringN(ClObjectHdr * hdr, const char *str, unsigned int length);
static void replaceClStringN(ClObjectHdr * hdr, int id, const char *str, unsigned int length);
			       
// static int insQualifier(ClObjectHdr * hdr, ClSection * qlfs,ClQualifier 
// 
// 
// * nq);

// static CMPIData _ClDataString(char *str, CMPIValueState s);
// static CMPIData _ClDataSint32(long n, CMPIValueState s);

static void     clearClSection(ClSection * s);
static void    *ensureClSpace(ClObjectHdr * hdr, ClSection * sct, int size,
                              int iSize);
extern void     dateTime2chars(CMPIDateTime *dt, CMPIStatus *rc,
                               char *str_time);
extern char    *sfcb_pathToChars(CMPIObjectPath * cop, CMPIStatus *rc,
                                 char *str);
extern void     dump(char *msg, void *a, int l);
extern CMPIArray *native_make_CMPIArray(CMPIData *av, CMPIStatus *rc,
                                        ClObjectHdr * hdr);
static ClString nls = { 0 };

ClVersionRecord
ClBuildVersionRecord(unsigned short opt, int endianMode, long *size)
{
  ClVersionRecord rec = { {{
                            {0},
                            0,
                            HDR_Version,
                            "sfcb-rep",
                            ClCurrentVersion,
                            ClCurrentLevel,
                            ClCurrentObjImplLevel,
                            opt, 0,
                            {0}}}
  };

  time_t          tm = time(NULL);

  rec.size = 0;
  rec.version = htons(rec.version);
  rec.level = htons(rec.level);
  rec.options = htons(rec.options);
  rec.objImplLevel = htons(rec.objImplLevel);
  strcpy(rec.creationDate, ctime(&tm));
  rec.creationDate[strlen(rec.creationDate) - 1] = 0;

  if (endianMode == SFCB_BIG_ENDIAN)
    rec.sByte[3] = sizeof(ClVersionRecord);
  else if (endianMode == SFCB_LITTLE_ENDIAN)
    rec.sByte[0] = sizeof(ClVersionRecord);
  else
    rec.size = sizeof(ClVersionRecord);

  *size = sizeof(ClVersionRecord);
  rec.lastInt = 0x25252525;

  return rec;
}

int
ClVerifyObjImplLevel(ClVersionRecord * vr)
{
  if (vr == NULL)
    return -1;

  vr->version = ntohs(vr->version);
  vr->level = ntohs(vr->level);
  vr->options = ntohs(vr->options);
  vr->objImplLevel = ntohs(vr->objImplLevel);

  return (vr->objImplLevel == ClCurrentObjImplLevel);
}

static void
clearClSection(ClSection * s)
{
  memset(s, 0, sizeof(*s));
}

const char     *
ClObjectGetClString(ClObjectHdr * hdr, ClString * id)
{
  ClStrBuf       *buf;

  if (id->id == 0)
    return NULL;
  buf = getStrBufPtr(hdr);
  return &(buf->buf[buf->indexPtr[id->id - 1]]);
}

const char     *
ClObjectGetClObject(ClObjectHdr * hdr, ClString * id)
{
  return (ClObjectGetClString(hdr, id));
}

const CMPIData *
ClObjectGetClArray(ClObjectHdr * hdr, ClArray * id)
{
  ClArrayBuf     *buf;

  if (id->id == 0)
    return NULL;
  buf = getArrayBufPtr(hdr);
  return &(buf->buf[buf->indexPtr[id->id - 1]]);
}

void           *
ClObjectGetClSection(ClObjectHdr * hdr, ClSection * s)
{
  return getSectionPtr(hdr, s);
}

static void    *
ensureClSpace(ClObjectHdr * hdr, ClSection * sct, int size, int iSize)
{
  void           *p;
  unsigned short  max = GetMax(sct->max);

  if (sct->sectionOffset == 0) {
    p = setSectionPtr(sct, malloc((sct->max = iSize) * size));
    hdr->flags |= HDR_Rebuild;
  } else if (sct->used >= max) {
    max *= 2;
    if (isMallocedSection(sct))
      setSectionPtr(sct,
                    realloc(sct->sectionPtr, (sct->max = max) * size));
    else {
      void           *f,
                     *t;
      f = ((char *) hdr) + sct->sectionOffset;
      t = malloc(max * size);
      memcpy(t,f,sct->used*size);
      sct->max = max;
      setSectionPtr(sct, t);
    }
    p = sct->sectionPtr;
    hdr->flags |= HDR_Rebuild;
  } else
    p = getSectionPtr(hdr, sct);

  return p;
}

long
addClString(ClObjectHdr * hdr, const char *str){
  return addClStringN(hdr, str, 0);
}

static long
addClStringN(ClObjectHdr * hdr, const char *str, unsigned int length)
{
  _SFCB_ENTER(TRACE_OBJECTIMPL, "addClString");

  unsigned int  nmax = 16,
                l;

  // see addClObject - hack to get anything into a stringbuffer
  if (length) {
    l = length;
  } else {
    l = strlen(str) + 1;
  }
  ClStrBuf       *buf;

  if (hdr->strBufOffset == 0) {
    nmax = 256;
    for (; nmax <= l; nmax *= 2);
    buf =
        setStrBufPtr(hdr,
                     malloc(((nmax - 1) * sizeof(char)) +
                            sizeof(ClStrBuf)));
    buf->bMax = nmax;
    buf->bUsed = buf->iUsed = 0;
    buf->iMax = 16;
    setStrIndexPtr(buf, malloc(sizeof(*buf->indexPtr) * 16));
    hdr->flags |= HDR_Rebuild;
  }

  else {
    int             malloced = isMallocedStrBuf(hdr);
    buf = getStrBufPtr(hdr);

    nmax = GetMax(buf->iMax);
    if (buf->iUsed >= nmax) {
      if (buf->iMax > 0) {
        if (!isMallocedStrIndex(buf)) {
          void           *idx = buf->indexPtr;
          buf->iMax = nmax * 2;
          setStrIndexPtr(buf, malloc(buf->iMax * sizeof(*buf->indexPtr)));
          memcpy(buf->indexPtr, idx, nmax * sizeof(*buf->indexPtr));
        } else {
          buf->iMax = nmax * 2;
	  setStrIndexPtr(buf, realloc(buf->indexPtr, buf->iMax * sizeof(*buf->indexPtr)));
        }
      } else {
        buf->iMax = 16;
        setStrIndexPtr(buf, malloc(buf->iMax * sizeof(*buf->indexPtr)));
      }
      hdr->flags |= HDR_Rebuild;
    }

    if (buf->bUsed + l >= buf->bMax) {
      nmax = buf->bMax;
      for (; nmax <= buf->bUsed + l; nmax *= 2);
      if (buf->bMax > 0) {
        if (!malloced) {
          setStrBufPtr(hdr,
                       malloc(((nmax - 1) * sizeof(char)) +
                              sizeof(ClStrBuf)));
          memcpy(hdr->strBuffer, buf, buf->bMax + sizeof(ClStrBuf));
        } else
          setStrBufPtr(hdr, realloc(hdr->strBuffer,
                                    ((nmax - 1) * sizeof(char)) +
                                    sizeof(ClStrBuf)));
      } else
        setStrBufPtr(hdr,
                     malloc(((nmax - 1) * sizeof(char)) +
                            sizeof(ClStrBuf)));
      buf = hdr->strBuffer;
      buf->bMax = nmax;
      hdr->flags |= HDR_Rebuild;
    }
  }

  // strcpy(buf->buf + buf->bUsed, str);
  memcpy(buf->buf + buf->bUsed, str, l);
  buf->indexPtr[buf->iUsed++] = buf->bUsed;
  buf->bUsed += l;

  _SFCB_RETURN(buf->iUsed);
}

// hack to get anything into a stringbuffer
static long
addClObject(ClObjectHdr * hdr, const void *obj, int size)
{
  long            retCode;
  retCode = addClStringN(hdr, obj,size);
  return (retCode);
}

/*
 * TODO: search for duplicate code between here and addObjectPropertyH() 
 */
static long
addClArray(ClObjectHdr * hdr, CMPIData d)
{
  _SFCB_ENTER(TRACE_OBJECTIMPL, "addClArray");

  struct native_array *ar = (struct native_array *) d.value.array;
  unsigned int    nmax = 16,
                  l = ar->size + 1,
                  i,
                  m;
  ClArrayBuf     *buf;
  CMPIData        td,
                 *dp;

  if (hdr->arrayBufOffset == 0) {
    nmax = 16;
    for (; nmax <= l; nmax *= 2);
    buf =
        setArrayBufPtr(hdr,
                       malloc(((nmax - 1) * sizeof(CMPIData)) +
                              sizeof(ClArrayBuf)));
    buf->bMax = nmax;
    buf->bUsed = buf->iUsed = 0;
    buf->iMax = 16;
    setArrayIndexPtr(buf, malloc(sizeof(*buf->indexPtr) * 16));
    hdr->flags |= HDR_Rebuild;
  }

  else {
    int             malloced = isMallocedArrayBuf(hdr);
    buf = getArrayBufPtr(hdr);

    nmax = GetMax(buf->iMax);
    if (buf->iUsed >= nmax) {
      if (buf->iMax > 0) {
        if (!isMallocedArrayIndex(buf)) {
          void           *idx = buf->indexPtr;
          buf->iMax = nmax * 2;
          setArrayIndexPtr(buf, malloc(buf->iMax * sizeof(*buf->indexPtr)));
          memcpy(buf->indexPtr, idx, nmax * sizeof(*buf->indexPtr));
        } else {
          buf->iMax = nmax * 2;
	  setArrayIndexPtr(buf, realloc(buf->indexPtr, buf->iMax * sizeof(*buf->indexPtr)));
        }
      } else {
        buf->iMax = 16;
	setArrayIndexPtr(buf, malloc(buf->iMax * sizeof(*buf->indexPtr)));
      }
      hdr->flags |= HDR_Rebuild;
    }

    if (buf->bUsed + l >= buf->bMax) {
      nmax = buf->bMax;
      for (; nmax <= buf->bUsed + l; nmax *= 2);
      if (buf->bMax > 0) {
        if (!malloced) {
          setArrayBufPtr(hdr,
                         malloc(((nmax - 1) * sizeof(CMPIData)) +
                                sizeof(ClArrayBuf)));
          memcpy((void *) hdr->arrayBufOffset, buf,
                 buf->bMax + sizeof(ClArrayBuf));
        } else
          setArrayBufPtr(hdr, realloc(hdr->arrayBuffer,
                                      ((nmax - 1) * sizeof(CMPIData)) +
                                      sizeof(ClArrayBuf)));
      } else
        setArrayBufPtr(hdr,
                       malloc(((nmax - 1) * sizeof(CMPIData)) +
                              sizeof(ClArrayBuf)));
      buf = hdr->arrayBuffer;
      buf->bMax = nmax;
      hdr->flags |= HDR_Rebuild;
    }
  }

  td.state = 0;
  // include array type, needed for objectImplSwapping in
  // copyI32toP32ArrayBuf
  td.type =
      ((ar->type == CMPI_string) ? CMPI_chars : ar->type) | CMPI_ARRAY;
  td.value.sint32 = ar->size;
  buf->indexPtr[buf->iUsed++] = buf->bUsed;
  buf->buf[buf->bUsed++] = td;

  for (i = 0, dp = buf->buf + buf->bUsed, m = ar->size; i < m; i++) {
    td.state = ar->data[i].state;
    if (ar->type == CMPI_chars && ((td.state & CMPI_nullValue) == 0)) {
      td.value.chars = (char *) addClString(hdr, ar->data[i].value.chars);
      td.type = CMPI_string;
    } else if (ar->type == CMPI_string
               && ((td.state & CMPI_nullValue) == 0)) {
      td.value.chars =
          (char *) addClString(hdr, ar->data[i].value.string->hdl);
      td.type = CMPI_string;
    } else if (ar->type == CMPI_dateTime
               && ((td.state & CMPI_nullValue) == 0)) {
      char            chars[26];
      dateTime2chars(ar->data[i].value.dateTime, NULL, chars);
      td.value.chars = (char *) addClString(hdr, chars);
      td.type = CMPI_dateTime;
    } else if (ar->type == CMPI_ref && ((td.state & CMPI_nullValue) == 0)) {
      char            str[4096] = { 0 };
      sfcb_pathToChars(ar->data[i].value.ref, NULL, str);
      td.value.chars = (char *) addClString(hdr, str);
      td.type = CMPI_ref;
    } else if (ar->type == CMPI_instance
               && ((td.state & CMPI_nullValue) == 0)) {
      int             size =
          getInstanceSerializedSize(ar->data[i].value.inst);
      void           *blob = malloc(size);
      getSerializedInstance(ar->data[i].value.inst, blob);
      td.value.inst = (CMPIInstance *) addClObject(hdr, blob, size);
      td.type = CMPI_instance;
      free(blob);
    } else {
      td.value = ar->data[i].value;
      td.type = ar->type & ~CMPI_ARRAY;
    }
    dp[i] = td;
  }

  buf->bUsed += m;

  _SFCB_RETURN(buf->iUsed);
}

static void
freeStringBuf(ClObjectHdr * hdr)
{
  _SFCB_ENTER(TRACE_OBJECTIMPL, "freeStringBuf");

  ClStrBuf       *buf;
  if (hdr->strBufOffset == 0)
    return;
  buf = getStrBufPtr(hdr);
  if (isMallocedStrIndex(buf))
    free(buf->indexPtr);
  if (isMallocedStrBuf(hdr))
    free(hdr->strBuffer);

  _SFCB_EXIT();
}

static void
freeArrayBuf(ClObjectHdr * hdr)
{
  _SFCB_ENTER(TRACE_OBJECTIMPL, "freeArrayBuf");

  ClArrayBuf     *buf;
  if (hdr->arrayBufOffset == 0)
    return;
  buf = getArrayBufPtr(hdr);
  if (isMallocedArrayIndex(buf))
    free(buf->indexPtr);
  if (isMallocedArrayBuf(hdr))
    free(hdr->arrayBuffer);

  _SFCB_EXIT();
}

static long
sizeStringBuf(ClObjectHdr * hdr)
{
  _SFCB_ENTER(TRACE_OBJECTIMPL, "sizeStringBuf");

  ClStrBuf       *buf;
  long            sz = 0;
  if (hdr->strBufOffset == 0)
    _SFCB_RETURN(0);

  buf = getStrBufPtr(hdr);

  sz = sizeof(*buf) + ALIGN(buf->bUsed,
                            4) + (buf->iUsed * sizeof(*buf->indexPtr));
  DEB(printf("--- sizeStringBuf: %lu-%p\n", sz, (void *) sz));

  _SFCB_RETURN(ALIGN(sz, CLALIGN));
}

static long
sizeArrayBuf(ClObjectHdr * hdr)
{
  _SFCB_ENTER(TRACE_OBJECTIMPL, "sizeArrayBuf");

  ClArrayBuf     *buf;
  long            sz = 0;
  if (hdr->arrayBufOffset == 0)
    _SFCB_RETURN(0);

  buf = getArrayBufPtr(hdr);

  sz = sizeof(*buf) + (buf->bUsed * sizeof(CMPIData)) +
      (buf->iUsed * sizeof(*buf->indexPtr));

  DEB(printf("--- sizeArrayBuf: %lu-%p\n", sz, (void *) sz));

  _SFCB_RETURN(ALIGN(sz, CLALIGN));
}

static void
replaceClString(ClObjectHdr * hdr, int id, const char *str){
  return replaceClStringN(hdr, id, str, 0);
}

static int getBufIndexLen(int *indexPtr, int bUsed, int iUsed, int index)
{
  /*
   * Find length of 'string' in fb at given index.
   * We cannot use strlen, because some items are not strings but embedded
   * instances.
   * We cannot simply substract fb->indexPtr[index+1] - fb->indexPtr[index],
   * because the entries are not consecutive! They are shuffled by
   * replaceClStringN().
   * Therefore the only way to find a length of our 'string' is to find
   * string, which starts immediately after it. Let's call it 'nearest'
   * string. */
  int nearest_start = bUsed;
  int our_start = indexPtr[index];
  int i;
  for (i = 0; i<iUsed; i++)
    if (indexPtr[i] > our_start && indexPtr[i] < nearest_start) {
      nearest_start = indexPtr[i];
    }
  int len = nearest_start - our_start;
  return len;
}

static void
replaceClStringN(ClObjectHdr * hdr, int id, const char *str, unsigned int length)
{
  _SFCB_ENTER(TRACE_OBJECTIMPL, "replaceClString");

  char           *ts,
                 *fs;
  long            i,
                  l,
                  u;
  ClStrBuf       *fb;
  int            *oldIndexPtr;

  fb = getStrBufPtr(hdr);
  ts = malloc(fb->bUsed);
  fs = &fb->buf[0];

  /* Copy indexPtr from the buffer, so we can compute lengths of items in it.*/
 oldIndexPtr = malloc(sizeof(*oldIndexPtr)*fb->iUsed);
 memcpy(oldIndexPtr, fb->indexPtr, sizeof(*oldIndexPtr)*fb->iUsed);

  for (u = i = 0; i < fb->iUsed; i++) {
    if (i != id - 1) {
      char           *f = fs + fb->indexPtr[i];
      l = getBufIndexLen(oldIndexPtr, fb->bUsed, fb->iUsed, i);
      fb->indexPtr[i] = u;
      memcpy(ts + u, f, l);
      u += l;
    }
  }
  memcpy(fs, ts, u);
  fb->bUsed = u;
  free(ts);
  free(oldIndexPtr);

  i = addClStringN(hdr, str, length);
  fb = getStrBufPtr(hdr);  /* addClString may change the strbufptr */
  fb->iUsed--;
  fb->indexPtr[id - 1] = fb->indexPtr[i - 1];

  _SFCB_EXIT();
}

/* Removes an object from the string buffer. The CMPIData pointing to that 
   object remains intact. A call to this function MUST be followed by a
   call to addClObject(), else one of the CMPIData entries will be invalid.

   id - the number of the entry to be removed   
 */
static void
removeClObject(ClObjectHdr * hdr, int id)
{
   _SFCB_ENTER(TRACE_OBJECTIMPL, "removeClObject");
   //   fprintf(stderr, "replaceClString: %p replacing entry for we're skipping %d\n", hdr, (id-1));
   char *ts, *fs;
   long i, l, u;
   ClStrBuf *fb;
   int *oldIndexPtr;

   fb = getStrBufPtr(hdr);   
   ts = malloc(fb->bUsed); /* tmp string buffer */
   fs = &fb->buf[0];
   /* Copy indexPtr from the buffer, so we can compute lengths of items in it.*/
   oldIndexPtr = malloc(sizeof(*oldIndexPtr)*fb->iUsed);
   memcpy(oldIndexPtr, fb->indexPtr, sizeof(*oldIndexPtr)*fb->iUsed);

   for (u = i = 0; i < fb->iUsed; i++) {
      if (i != id - 1) {    /* loop through and copy over all _other_ properties */
        //      fprintf(stderr, "replace: keeping %ld\n", i);
         char *f = fs + fb->indexPtr[i];
	 l = getBufIndexLen(oldIndexPtr, fb->bUsed, fb->iUsed, i);

         /* Bugzilla 74159 - Align the string buffer & null terminate */
         /*if (l % sizeof(long) != 0) {
           l = ALIGN((fb->indexPtr[i+1] - fb->indexPtr[i]), CLALIGN);
           tmpstr = calloc(1,l);
           if (tmpstr == NULL) {
             _SFCB_TRACE(1, ("objectImpl:replaceClString: calloc failed for tmpstr"));
           }
           memcpy(tmpstr, f, l);
         } */

         fb->indexPtr[i] = u;

         /*if (tmpstr != NULL) {
           memcpy(ts + u, tmpstr, l);
           free(tmpstr); 
           tmpstr = NULL;
         }
         else */
           memcpy(ts + u, f, l);

         u += l;
      }
   }
   memcpy(fs, ts, u);
   fb->bUsed = u;
   free(ts);
   free(oldIndexPtr);

   fb->iUsed--; /* fixup the item count, since we have one fewer elements */

  _SFCB_EXIT();
}

// hack to get anything into a stringbuffer
static void
replaceClObject(ClObjectHdr * hdr, int id, const void *obj, int size)
{
  replaceClStringN(hdr, id, obj, size);
}

/*
 * replace array currently at position id in object hdr with array d
 */
static void
replaceClArray(ClObjectHdr * hdr, int id, CMPIData d)
{
  _SFCB_ENTER(TRACE_OBJECTIMPL, "replaceClArray");

  CMPIData       *ts,  /* hold the arrays we don't need to replace */
                 *fs;  /* hold the address of hdr's ArrayBuf */
  long            i,
                  l,
                  u;
  ClArrayBuf     *fb;  /* the arrayBuf of this object (hdr) */

  fb = getArrayBufPtr(hdr);
  ts = malloc(fb->bUsed * sizeof(*ts));
  fs = &fb->buf[0];

  /* copy the arrays in fb that /aren't/ being replaced */
  for (u = i = 0; i < fb->iUsed; i++) {
    if (i != id - 1) {
      CMPIData       *f = fs + fb->indexPtr[i];  /* get the array at this position */
      l = (f->value.sint32 + 1) * sizeof(CMPIData);  /* f->value.sint32 is the number of members in current array */
      fb->indexPtr[i] = u;
      memcpy(ts + u, f, l);   /* copy array into temporary space */
      u += f->value.sint32 + 1;
    }
  }
  memcpy(fs, ts, u * sizeof(CMPIData));   /* now copy all temp stuff into real buffer */
  fb->bUsed = u;
  free(ts);

  i = addClArray(hdr, d);  /* and add the new array */
  fb->iUsed--;
  fb->indexPtr[id - 1] = fb->indexPtr[i-1];   /* the position where the CMPI array at id starts internally */

  _SFCB_EXIT();
}

static int
copyStringBuf(int ofs, ClObjectHdr * th, ClObjectHdr * fh)
{
  _SFCB_ENTER(TRACE_OBJECTIMPL, "copyStringBuf");

  ClStrBuf       *fb,
                 *tb;
  long            l,
                  il;

  if (fh->strBufOffset == 0)
    _SFCB_RETURN(0);

  tb = (ClStrBuf *) (((char *) th) + ofs);
  fb = getStrBufPtr(fh);

  l = sizeof(*fb) + fb->bUsed;
  il = fb->iUsed * sizeof(*fb->indexPtr);

  memcpy(tb, fb, l);
  tb->bMax = tb->bUsed;
  setStrBufOffset(th, ofs);
  l = ALIGN(l, 4);
  ofs += l;

  memcpy(((char *) th) + ofs, fb->indexPtr, il);
  tb->iMax = tb->iUsed;
  setStrIndexOffset(th, tb, ofs);

  _SFCB_RETURN(ALIGN(l + il, CLALIGN));
}

static int
copyArrayBuf(int ofs, ClObjectHdr * th, ClObjectHdr * fh)
{
  _SFCB_ENTER(TRACE_OBJECTIMPL, "copyArrayBuf");

  ClArrayBuf     *fb,
                 *tb;
  long            l,
                  il;

  if (fh->arrayBufOffset == 0)
    _SFCB_RETURN(0);

  tb = (ClArrayBuf *) (((char *) th) + ofs);
  fb = getArrayBufPtr(fh);

  l = sizeof(*fb) + (fb->bUsed * sizeof(CMPIData));
  il = fb->iUsed * sizeof(*fb->indexPtr);

  memcpy(tb, fb, l);
  tb->bMax = tb->bUsed;
  setArrayBufOffset(th, ofs);
  ofs += l;

  memcpy(((char *) th) + ofs, fb->indexPtr, il);
  tb->iMax = tb->iUsed;
  setArrayIndexOffset(th, tb, ofs);

  _SFCB_RETURN(ALIGN(l + il, CLALIGN));
}

// string concatenation

static char    *
cat2string(stringControl * sc, const char *str)
{
  unsigned int    nmax,
                  l = strlen(str) + 1;
  if (str) {
    if (sc->str == NULL) {
      for (nmax = sc->max; nmax <= sc->used + l; nmax *= 2);
      sc->str = malloc((sc->max = nmax));
    } else if (l + sc->used >= sc->max) {
      for (nmax = sc->max; nmax <= sc->used + l; nmax *= 2);
      sc->str = realloc(sc->str, (sc->max = nmax));
    }
    strcpy(sc->str + sc->used, str);
    sc->used += l - 1;
  }
  return sc->str;
}

// -------------------------------------------------------
// -----
// -- data support
// -----
// -------------------------------------------------------

static char    *
datatypeToString(CMPIData *d, char **array)
{
  static char    *Array = "[]";
  CMPIType        type = d->type & ~CMPI_ARRAY;

  if (d->type & CMPI_ARRAY)
    *array = Array;
  else
    *array = NULL;

  switch (type) {
  case CMPI_chars:
  case CMPI_string:
    return "string";
  case CMPI_real64:
    return "real64";
  case CMPI_real32:
    return "real32";
  case CMPI_sint64:
    return "sint64";
  case CMPI_uint64:
    return "uint64";
  case CMPI_sint32:
    return "sint32";
  case CMPI_uint32:
    return "uint32";
  case CMPI_sint16:
    return "sint16";
  case CMPI_uint16:
    return "uint16";
  case CMPI_uint8:
    return "uint8";
  case CMPI_sint8:
    return "sint8";
  case CMPI_boolean:
    return "boolean";
  case CMPI_char16:
    return "char16";
  case CMPI_ref:
    return "ref";
  case CMPI_instance:
    return "instance";
  case CMPI_dateTime:
    return "dateTime";
  default:
    return "unknownType";
  }
}

/*
 * print a value into a string based on fmt format string
 */
static char    *
fmtstr(const char *fmt, ...)
{
  va_list         ap;
  int             len;
  char           *str;

  va_start(ap, fmt);
  len = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if (len <= 0) {
    return NULL;
  }
  str = malloc(len + 1);
  if (str == NULL) {
    return NULL;
  }
  va_start(ap, fmt);
  vsnprintf(str, len + 1, fmt, ap);
  va_end(ap);
  return str;
}

/*
 * Translate a CMPIData value to a char*
 */
static char    *
dataValueToString(ClObjectHdr * hdr, CMPIData *d)
{
  static char    *True = "true",
      *False = "false";
  char           *retval = NULL;
  switch (d->type) {
  case CMPI_chars:
    {
      char           *ret =
          (char *) ClObjectGetClString(hdr, (ClString *) & d->value.chars);
      if (ret)
        retval = fmtstr("%s", ret);
      else
        retval = strdup("");
    }
    break;
  case CMPI_char16:
    {
      char           *ret = (char *) ClObjectGetClString(hdr,
                                                         (ClString *) &
                                                         d->value.char16);
      if (ret)
        retval = fmtstr("%s", ret);
      else
        retval = strdup("");
    }
    break;
  case CMPI_real64:
    retval = fmtstr("%lf", d->value.real64);
    break;
  case CMPI_real32:
    retval = fmtstr("%f", d->value.real32);
    break;
  case CMPI_sint64:
    retval = fmtstr("%lld", d->value.sint64);
    break;
  case CMPI_sint32:
    retval = fmtstr("%d", d->value.sint32);
    break;
  case CMPI_sint16:
    retval = fmtstr("%d", d->value.sint16);
    break;
  case CMPI_sint8:
    retval = fmtstr("%d", d->value.sint8);
    break;
  case CMPI_uint64:
    retval = fmtstr("%llu", d->value.uint64);
    break;
  case CMPI_uint32:
    retval = fmtstr("%u", d->value.uint32);
    break;
  case CMPI_uint16:
    retval = fmtstr("%u", d->value.uint16);
    break;
  case CMPI_uint8:
    retval = fmtstr("%u", d->value.uint8);
    break;
  case CMPI_boolean:
    retval = fmtstr("%s", d->value.boolean ? True : False);
    break;
  default:
    retval = strdup("***??***");
  }
  return retval;
}

// -------------------------------------------------------
// -----
// -- qualifier support
// -----
// -------------------------------------------------------

static int
locateQualifier(ClObjectHdr * hdr, ClSection * qlfs, const char *id)
{
  int             i;
  ClQualifier    *q;

  q = (ClQualifier *) getSectionPtr(hdr, qlfs);

  for (i = 0; i < qlfs->used; i++) {
    if (strcasecmp(id, ClObjectGetClString(hdr, &(q + i)->id)) == 0)
      return i + 1;
  }
  return 0;
}

static int
addClQualifier(ClObjectHdr * hdr, ClSection * qlfs, const char *id,
               CMPIData d, ClObjectHdr * arrHdr)
{
  int             i;
  ClQualifier     nq,
                 *q;
  CMPIStatus      st;
  CMPIArray      *ar = NULL;

  if ((i = locateQualifier(hdr, qlfs, id)) == 0) {
    nq.id.id = addClString(hdr, id);
    if (d.type == CMPI_chars && (d.state & CMPI_nullValue) == 0) {
      d.value.chars = (char *) addClString(hdr, d.value.chars);
    } else if (d.type == CMPI_string && (d.state & CMPI_nullValue) == 0) {
      d.type = CMPI_chars;
      d.value.chars =
          (char *) addClString(hdr, (char *) d.value.string->hdl);
    } else if ((d.type & CMPI_ARRAY) && (d.state & CMPI_nullValue) == 0) {
      if (arrHdr) {
        ar = d.value.array =
            native_make_CMPIArray((CMPIData *) d.value.inst, &st, arrHdr);
      }
      d.value.array = (CMPIArray *) addClArray(hdr, d);
      if (ar)
        ar->ft->release(ar);
    }

    nq.data = d;
    q = (ClQualifier *) ensureClSpace(hdr, qlfs, sizeof(*q), 4);
    q = q + (qlfs->used++);
    *q = nq;
    return qlfs->used;
  }
  return 0;
}

int
ClClassAddQualifierSpecial(ClObjectHdr * hdr, ClSection * qlfs,
                           const char *id, CMPIData d,
                           ClObjectHdr * arrHdr)
{

  if (hdr->type == HDR_Class) {
    ClClass        *cls = (ClClass *) hdr;
    if (strcasecmp(id, "Abstract") == 0)
      cls->quals |= ClClass_Q_Abstract;
    else if (strcasecmp(id, "Association") == 0)
      cls->quals |= ClClass_Q_Association;
    else if (strcasecmp(id, "Indication") == 0)
      cls->quals |= ClClass_Q_Indication;
    else {
      return addClQualifier(hdr, qlfs, id, d, arrHdr);
    }
    return 0;
  }

  return addClQualifier(hdr, qlfs, id, d, arrHdr);
}

int
ClClassAddQualifier(ClObjectHdr * hdr, ClSection * qlfs, const char *id,
                    CMPIData d)
{
  return ClClassAddQualifierSpecial(hdr, qlfs, id, d, 0);
}

int
ClClassAddPropertyQualifierSpecial(ClObjectHdr * hdr, ClProperty * p,
                                   const char *id, CMPIData d,
                                   ClObjectHdr * arrHdr)
{
  if (strcasecmp(id, "key") == 0)
    p->quals |= ClProperty_Q_Key;
  else if (strcasecmp(id, "embeddedobject") == 0)
    p->quals |= ClProperty_Q_EmbeddedObject;
  /* Make EmbeddedInstance behave as EmbeddedObject, but be able to distinguish between them */
  else if (strcasecmp(id, "embeddedinstance") == 0) {
    p->quals |= ClProperty_Q_EmbeddedObject;
    p->quals |= ClProperty_Q_EmbeddedInstance;
  } else
    return ClClassAddQualifierSpecial(hdr, &p->qualifiers, id, d, arrHdr);
  return 0;
}

int
ClClassAddPropertyQualifier(ClObjectHdr * hdr, ClProperty * p,
                            const char *id, CMPIData d)
{
  return ClClassAddPropertyQualifierSpecial(hdr, p, id, d, NULL);
}

int
ClClassAddMethodQualifier(ClObjectHdr * hdr, ClMethod * m,
                          const char *id, CMPIData d)
{
  // ClQualifier q;
  // q = makeClQualifier(hdr, id, d);
  return addClQualifier(hdr, &m->qualifiers, id, d, 0);

}

int
ClClassAddMethParamQualifier(ClObjectHdr * hdr, ClParameter * p,
                             const char *id, CMPIData d)
{
  // ClQualifier q;
  // q = makeClQualifier(hdr, id, d);
  return addClQualifier(hdr, &p->qualifiers, id, d, 0);
}

static void
freeQualifiers(ClSection * s)
{
  if (isMallocedSection(s))
    free(s->sectionPtr);
}

static char    *
addQualifierToString(stringControl * sc, ClObjectHdr * hdr,
                     ClQualifier * q, int sb)
{
  int             o = sc->used;

  if (sb & 2)
    cat2string(sc, "  [");
  else
    cat2string(sc, ", ");
  cat2string(sc, ClObjectGetClString(hdr, &q->id));
  if (q->data.state != CMPI_nullValue) {
    cat2string(sc, "(");
    char           *val = dataValueToString(hdr, &q->data);
    cat2string(sc, val);
    cat2string(sc, ")");
    free(val);
  }
  if (sb & 1)
    cat2string(sc, "]");
  return sc->str + o;
}

static long
sizeQualifiers(ClSection * s)
{
  long            sz;

  sz = s->used * sizeof(ClQualifier);
  return ALIGN(sz, CLALIGN);
}

static int
copyQualifiers(int ofs, char *to, ClSection * ts,
               ClObjectHdr * from, ClSection * fs)
{
  ClQualifier    *q;
  int             l = ts->used * sizeof(ClQualifier);
  if (l == 0)
    return 0;
  ts->max = ts->used;

  q = (ClQualifier *) ClObjectGetClSection(from, fs);

  memcpy(to + ofs, q, l);
  setSectionOffset(ts, ofs);
  return ALIGN(l, CLALIGN);
}

static int
ClGetQualifierAt(ClClass * cls, ClQualifier * q, int id, CMPIData *data,
                 char **name)
{
  if (data) {
    *data = (q + id)->data;
    if (data->type == CMPI_chars) {
      const char     *str =
          ClObjectGetClString(&cls->hdr, (ClString *) & data->value.chars);
      data->value.string = sfcb_native_new_CMPIString(str, NULL, 0);
      data->type = CMPI_string;
    } else if (data->type & CMPI_ARRAY) {
      data->value.dataPtr.ptr = (void *) ClObjectGetClArray(&cls->hdr,
                                                            (ClArray *) &
                                                            data->value.
                                                            array);
    }
  }
  if (name)
    *name = (char *) ClObjectGetClString(&cls->hdr, &(q + id)->id);
  return 0;
}

int

ClClassGetQualifierAt(ClClass * cls, int id, CMPIData *data, char **name)
{
  ClQualifier    *q;
  q = (ClQualifier *) ClObjectGetClSection(&cls->hdr, &cls->qualifiers);
  if (id < 0 || id > cls->qualifiers.used)
    return 1;
  return ClGetQualifierAt(cls, q, id, data, name);
}

int
ClClassGetQualifierCount(ClClass * cls)
{
  ClObjectGetClSection(&cls->hdr, &cls->qualifiers);
  return cls->qualifiers.used;
}

// -------------------------------------------------------
// -----
// -- parameter support
// -----
// -------------------------------------------------------

static int
locateParameter(ClObjectHdr * hdr, ClSection * prms, const char *id)
{
  int             i;
  ClParameter    *p;

  p = (ClParameter *) getSectionPtr(hdr, prms);

  for (i = 0; i < prms->used; i++) {
    if (strcasecmp(id, ClObjectGetClString(hdr, &(p + i)->id)) == 0)
      return i + 1;
  }
  return 0;
}

int
ClClassGetMethParamQualifierCount(ClParameter * p)
{
  return p->qualifiers.used;
}

static int
addClParameter(ClObjectHdr * hdr, ClSection * prms, ClParameter * np)
{
  int             i;
  ClParameter    *p;

  if ((i =
       locateParameter(hdr, prms,
                       ClObjectGetClString(hdr, &np->id))) == 0) {
    p = (ClParameter *) ensureClSpace(hdr, prms, sizeof(*p), 4);
    p = p + (prms->used++);
    *p = *np;
    return prms->used;
  }
  return i;
}

static          ClParameter
makeClParameter(ClObjectHdr * hdr, const char *id, CMPIParameter cp)
{
  ClParameter     p;

  memset(&p, 0, sizeof(p));
  p.id.id = addClString(hdr, id);
  if (cp.refName) {
    cp.refName = (char *) addClString(hdr, cp.refName);
  }
  p.parameter = cp;
  return p;
}

int
ClClassAddMethParameter(ClObjectHdr * hdr, ClMethod * m,
                        const char *id, CMPIParameter cp)
{
  ClParameter     p;

  p = makeClParameter(hdr, id, cp);
  return addClParameter(hdr, &m->parameters, &p);
}

static long
sizeParameters(ClObjectHdr * hdr, ClSection * s)
{
  int             l;
  long            sz = s->used * sizeof(ClParameter);
  ClParameter    *p = (ClParameter *) ClObjectGetClSection(hdr, s);

  for (l = s->used; l > 0; l--, p++) {
    if (p->qualifiers.used)
      sz += sizeQualifiers(&p->qualifiers);
  }
  return ALIGN(sz, CLALIGN);
}

static long
copyParameters(int ofs, char *to, ClSection * ts,
               ClObjectHdr * from, ClSection * fs)
{
  ClParameter    *fp = (ClParameter *) ClObjectGetClSection(from, fs);
  ClParameter    *tp = (ClParameter *) (to + ofs);
  int             i;
  int             l = ts->used * sizeof(ClParameter);
  if (l == 0)
    return 0;
  ts->max = ts->used;

  memcpy(tp, fp, l);
  setSectionOffset(ts, ofs);

  for (i = ts->used; i > 0; i--, fp++, tp++) {
    if (tp->qualifiers.used)
      l += copyQualifiers(ofs + l, to, &tp->qualifiers, from,
                          &fp->qualifiers);
  }

  return ALIGN(l, CLALIGN);
}

static void
freeParameters(ClSection * s)
{
  _SFCB_ENTER(TRACE_OBJECTIMPL, "freeParameters");

  if (isMallocedSection(s))
    free(s->sectionPtr);

  _SFCB_EXIT();
}

// -------------------------------------------------------
// -----
// -- method support
// -----
// -------------------------------------------------------

int
ClClassLocateMethod(ClObjectHdr * hdr, ClSection * mths, const char *id)
{
  int             i;
  ClMethod       *m;

  m = (ClMethod *) getSectionPtr(hdr, mths);

  for (i = 0; i < mths->used; i++) {
    if (strcasecmp(id, ClObjectGetClString(hdr, &(m + i)->id)) == 0)
      return i + 1;
  }
  return 0;
}

static int
addClassMethodH(ClObjectHdr * hdr, ClSection * mths,
                const char *id, CMPIType t)
{
  int             i;
  ClMethod       *m;

  _SFCB_ENTER(TRACE_OBJECTIMPL, "addClassMethodH");

  if ((i = ClClassLocateMethod(hdr, mths, id)) == 0) {
    m = (ClMethod *) ensureClSpace(hdr, mths, sizeof(*m), 8);
    m = m + (mths->used++);
    clearClSection(&m->qualifiers);
    clearClSection(&m->parameters);
    m->id.id = addClString(hdr, id);
    m->quals = 0;
    m->originId = 0;
    m->type = t;
    _SFCB_RETURN(mths->used);
  }

  else {
    m = (ClMethod *) getSectionPtr(hdr, mths);
    (m + i - 1)->type = t;
    _SFCB_RETURN(i);
  }
}

static long
sizeMethods(ClObjectHdr * hdr, ClSection * s)
{
  int             l;
  long            sz = s->used * sizeof(ClMethod);
  ClMethod       *m = (ClMethod *) ClObjectGetClSection(hdr, s);

  for (l = s->used; l > 0; l--, m++) {
    if (m->qualifiers.used)
      sz += sizeQualifiers(&m->qualifiers);
    if (m->parameters.used)
      sz += sizeParameters(hdr, &m->parameters);
  }
  DEB(printf("--- sizeMethods: %lu-%p\n", sz, (void *) sz));
  return ALIGN(sz, CLALIGN);
}

static long
copyMethods(int ofs, char *to, ClSection * ts,
            ClObjectHdr * from, ClSection * fs)
{
  ClMethod       *fp = (ClMethod *) ClObjectGetClSection(from, fs);
  ClMethod       *tp = (ClMethod *) (to + ofs);
  int             i;
  int             l = ts->used * sizeof(ClMethod);
  if (l == 0)
    return 0;
  ts->max = ts->used;

  memcpy(tp, fp, l);
  setSectionOffset(ts, ofs);

  for (i = ts->used; i > 0; i--, fp++, tp++) {
    if (tp->qualifiers.used)
      l += copyQualifiers(ofs + l, to, &tp->qualifiers, from,
                          &fp->qualifiers);
    if (tp->parameters.used)
      l += copyParameters(ofs + l, to, &tp->parameters, from,
                          &fp->parameters);
  }

  return ALIGN(l, CLALIGN);
}

static void
freeMethod(ClMethod * m)
{
  _SFCB_ENTER(TRACE_OBJECTIMPL, "freeMethod");

  if (m)
    freeQualifiers(&(m)->qualifiers);
  if (m)
    freeParameters(&(m)->parameters);

  _SFCB_EXIT();
}

static void
freeMethods(ClObjectHdr * hdr, ClSection * s, int meths)
{
    if (meths > 0) {
      int m=0;
      ClMethod* m_sec = (ClMethod *) ClObjectGetClSection(hdr, s);

      for (; m < meths; m++) {
	freeMethod(m_sec+m);
      }
      if (isMallocedSection(s))
	free(s->sectionPtr);
    }
}

int
ClClassGetMethQualifierCount(ClClass * cls, int id)
{
  ClMethod       *m;

  m = (ClMethod *) ClObjectGetClSection(&cls->hdr, &cls->methods);
  if (id < 0 || id > cls->methods.used)
    return -1;
  return (m + id)->qualifiers.used;
}

int
ClClassGetMethParameterCount(ClClass * cls, int id)
{
  ClMethod       *m;

  m = (ClMethod *) ClObjectGetClSection(&cls->hdr, &cls->methods);
  if (id < 0 || id > cls->methods.used)
    return -1;
  return (m + id)->parameters.used;
}

int
ClClassGetMethParmQualifierCount(ClClass * cls, ClMethod * m, int id)
{
  ClParameter    *p;

  p = (ClParameter *) ClObjectGetClSection(&cls->hdr, &m->parameters);
  if (id < 0 || id > p->qualifiers.used)
    return -1;
  return (p + id)->qualifiers.used;
}

int
ClClassGetMethParamQualifierAt(ClClass * cls, ClParameter * parm, int id,
                               CMPIData *data, char **name)
{
  ClQualifier    *q;
  q = (ClQualifier *) ClObjectGetClSection(&cls->hdr, &parm->qualifiers);
  if (id < 0 || id > parm->qualifiers.used)
    return 1;

  ClGetQualifierAt(cls, q, id, data, name);

  if (data && data->type & CMPI_ARRAY && data->value.array) {
    data->value.array =
        native_make_CMPIArray((CMPIData *) data->value.array, NULL,
                              &cls->hdr);
  }

  return 0;
}

// -------------------------------------------------------
// -----
// -- Class/Instance Property support
// -----
// -------------------------------------------------------

int
ClObjectLocateProperty(ClObjectHdr * hdr, ClSection * prps, const char *id)
{
  int             i;
  ClProperty     *p;

  p = (ClProperty *) getSectionPtr(hdr, prps);
  for (i = 0; i < prps->used; i++) {
    if (strcasecmp(id, ClObjectGetClString(hdr, &(p + i)->id)) == 0)
      return i + 1;
  }
  return 0;
}

static void
freeProperties(ClObjectHdr * hdr, ClSection * s)
{
  ClProperty     *p;
  int             i,
                  l;
  _SFCB_ENTER(TRACE_OBJECTIMPL, "freeProperties");

  p = (ClProperty *) ClObjectGetClSection(hdr, s);

  if (p)
    for (i = 0, l = s->used; i < l; i++) {
      freeQualifiers(&(p + i)->qualifiers);
    }
  if (isMallocedSection(s))
    free(s->sectionPtr);

  _SFCB_EXIT();
}

static char    *
addPropertyToString(stringControl * sc, ClObjectHdr * hdr, ClProperty * p)
{
  int             l,
                  i,
                  sb = 2,
      o = sc->used;
  ClQualifier    *q;
  char           *array = NULL;

  if (p->data.state && CMPI_nullValue)
    return NULL;

  q = (ClQualifier *) ClObjectGetClSection(hdr, &p->qualifiers);
  if ((l = p->qualifiers.used)) {
    for (i = 0; i < l; sb = 0, i++) {
      if (i == l - 1)
        sb |= 1;
      addQualifierToString(sc, hdr, q + i, sb);
    }
    cat2string(sc, "\n");
  }

  cat2string(sc, " ");
  cat2string(sc, datatypeToString(&p->data, &array));
  cat2string(sc, " ");
  cat2string(sc, ClObjectGetClString(hdr, &p->id));
  if (array)
    cat2string(sc, array);
  cat2string(sc, " = ");

  char           *val = dataValueToString(hdr, &p->data);
  if (val == NULL || *val == 0)
    cat2string(sc, "\"\"");
  else
    cat2string(sc, val);

  cat2string(sc, ";\n");
  free(val);
  return sc->str + o;
}

static long
sizeProperties(ClObjectHdr * hdr, ClSection * s)
{
  int             l;
  long            sz = s->used * sizeof(ClProperty);
  ClProperty     *p = (ClProperty *) ClObjectGetClSection(hdr, s);

  for (l = s->used; l > 0; l--, p++) {
    if (p->qualifiers.used)
      sz += sizeQualifiers(&p->qualifiers);
  }
  return ALIGN(sz, CLALIGN);
}

static long
copyProperties(int ofs, char *to, ClSection * ts,
               ClObjectHdr * from, ClSection * fs)
{
  ClProperty     *fp = (ClProperty *) ClObjectGetClSection(from, fs);
  ClProperty     *tp = (ClProperty *) (to + ofs);
  int             i;
  int             l = ts->used * sizeof(ClProperty);
  if (l == 0)
    return 0;
  ts->max = ts->used;

  memcpy(tp, fp, l);
  setSectionOffset(ts, ofs);

  for (i = ts->used; i > 0; i--, fp++, tp++)
    if (tp->qualifiers.used)
      l += copyQualifiers(ofs + l, to, &tp->qualifiers, from,
                          &fp->qualifiers);

  return ALIGN(l, CLALIGN);
}

void
showClHdr(void *ihdr)
{
  struct obj {
    ClObjectHdr    *hdr;
  }              *x;

  x = ihdr;
  printf("ClObjectHdr: %p->%p\n", ihdr, x->hdr);
  printf("\tsize:  %u\n", x->hdr->size);
  printf("\ttype:  %d\n", x->hdr->type);
  printf("\tflags: %d\n", x->hdr->flags);
  printf("\tsbo:   %p-%ld\n", (void *) x->hdr->strBufOffset,
         x->hdr->strBufOffset);
  printf("\tabo:   %p-%ld\n", (void *) x->hdr->arrayBufOffset,
         x->hdr->arrayBufOffset);
}

static int
addObjectPropertyH(ClObjectHdr * hdr, ClSection * prps,
                   const char *id, CMPIData d, char *refName)
{
  int             i;
  ClProperty     *p;
  CMPIData        od;
  CMPIStatus      st;

  _SFCB_ENTER(TRACE_OBJECTIMPL, "addObjectPropertyH");
  /*
   * if id is not in the section (prps), add it, increment number of props 
   */
  if ((i = ClObjectLocateProperty(hdr, prps, id)) == 0) {
    p = (ClProperty *) ensureClSpace(hdr, prps, sizeof(*p), 8);
    p = p + (prps->used++);
    clearClSection(&p->qualifiers);
    p->id.id = addClString(hdr, id);
    p->quals = p->flags = 0;
    p->originId = 0;
    if (refName) {
      p->refName.id = addClString(hdr, refName);
    } else {
      p->refName.id = 0;
    }

    if (d.type == CMPI_chars && (d.state & CMPI_nullValue) == 0) {
      p->data = d;
      p->data.value.chars = (char *) addClString(hdr, d.value.chars);
    } else if (d.type == CMPI_string && (d.state & CMPI_nullValue) == 0) {
      p->data = d;
      if (d.value.string)
        p->data.value.chars =
            (char *) addClString(hdr, (char *) d.value.string->hdl);
      else
        p->data.value.chars = NULL;
      p->data.type = CMPI_chars;
    } else if (d.type == CMPI_dateTime && (d.state & CMPI_nullValue) == 0) {
      char            chars[26];
      dateTime2chars(d.value.dateTime, NULL, chars);
      p->data = d;
      p->data.value.chars = (char *) addClString(hdr, chars);
    } else if (d.type == CMPI_ref && (d.state & CMPI_nullValue) == 0) {
      char            str[4096] = { 0 };
      sfcb_pathToChars(d.value.ref, &st, str);
      p->data = d;
      p->data.value.chars = (char *) addClString(hdr, str);
    } else if ((d.type & CMPI_ARRAY) && (d.state & CMPI_nullValue) == 0) {
      p->data = d;
      p->data.value.array = (CMPIArray *) addClArray(hdr, d);
    } else if (d.type == CMPI_instance && (d.state & CMPI_nullValue) == 0) {
      p->data = d;
      int             size = getInstanceSerializedSize(d.value.inst);
      void           *blob = malloc(size);
      getSerializedInstance(d.value.inst, blob);
      p->data.value.inst = (CMPIInstance *) addClObject(hdr, blob, size);
      free(blob);
      hdr->flags |= HDR_ContainsEmbeddedObject;
      p->quals |= ClProperty_Q_EmbeddedObject;
      p->flags &= ~ClProperty_EmbeddedObjectAsString;
    } else
      p->data = d;
    _SFCB_RETURN(prps->used);
  }

  else {
    p = (ClProperty *) getSectionPtr(hdr, prps);
    od = (p + i - 1)->data;

    if (od.type == CMPI_chars && (d.state & CMPI_nullValue) == 0) {

      if ((p + i - 1)->quals && ClProperty_Q_EmbeddedObject)
        _SFCB_RETURN(-CMPI_RC_ERR_TYPE_MISMATCH);

      if (d.type != CMPI_chars) {
        if (d.type != CMPI_string) {
          _SFCB_RETURN(-CMPI_RC_ERR_TYPE_MISMATCH);
        }
        replaceClString(hdr, (long) od.value.chars, "");
        (p + i - 1)->data = d;
      } else if (od.value.chars) {
        (p + i - 1)->data = d;
        replaceClString(hdr, (long) od.value.chars, d.value.chars);
        (p + i - 1)->data.value.chars = od.value.chars;
      } else {
        (p + i - 1)->data = d;
        (p + i - 1)->data.value.chars =
            (char *) addClString(hdr, d.value.chars);
      }
    }
    /* added from 2778345 */
    else if (od.type == CMPI_string && (d.state & CMPI_nullValue) == 0) {
      char *pClString = NULL;
      if ((p + i - 1)->quals && ClProperty_Q_EmbeddedObject)
        _SFCB_RETURN(-CMPI_RC_ERR_TYPE_MISMATCH);

      switch (d.type) {
      case CMPI_chars:
        pClString = d.value.chars;
        break;
      case CMPI_string:
        pClString = d.value.string->hdl;
        break;
      default:
        _SFCB_RETURN(-CMPI_RC_ERR_TYPE_MISMATCH);
      }
      if (pClString) {
        if (od.value.string) {
          _SFCB_RETURN(-CMPI_RC_ERR_TYPE_MISMATCH);
        }
        (p + i - 1)->data = d;
        (p + i - 1)->data.value.chars =
          (char *) addClString(hdr, pClString);
        (p + i - 1)->data.type=CMPI_chars;
      }
    }

    else if (od.type == CMPI_dateTime && (d.state & CMPI_nullValue) == 0) {
      char            chars[26];
      if (d.type != CMPI_dateTime) {
        _SFCB_RETURN(-CMPI_RC_ERR_TYPE_MISMATCH);
      }
      dateTime2chars(d.value.dateTime, NULL, chars);
      if (od.value.chars) {
        (p + i - 1)->data = d;
        replaceClString(hdr, (long) od.value.chars, chars);
        (p + i - 1)->data.value.chars = od.value.chars;
      } else {
        (p + i - 1)->data = d;
        (p + i - 1)->data.value.chars = (char *) addClString(hdr, chars);
      }
    }

    else if (od.type == CMPI_ref && (d.state & CMPI_nullValue) == 0) {
      char            chars[4096] = { 0 };
      if (d.type != CMPI_ref) {
        _SFCB_RETURN(-CMPI_RC_ERR_TYPE_MISMATCH);
      }
      sfcb_pathToChars(d.value.ref, &st, chars);
      if (od.value.chars) {
        (p + i - 1)->data = d;
        replaceClString(hdr, (long) od.value.chars, chars);
        (p + i - 1)->data.value.chars = od.value.chars;
      } else {
        (p + i - 1)->data = d;
        (p + i - 1)->data.value.chars = (char *) addClString(hdr, chars);
      }
    }

    else if ((od.type & CMPI_ARRAY) && (d.state & CMPI_nullValue) == 0) {
      if (!(d.type & CMPI_ARRAY)) {
        _SFCB_RETURN(-CMPI_RC_ERR_TYPE_MISMATCH);
      }
      if (od.value.array) {
        (p + i - 1)->data = d;
        replaceClArray(hdr, (long) od.value.array, d);
        (p + i - 1)->data.value.array = od.value.array;
      } else {
        (p + i - 1)->data = d;
        (p + i - 1)->data.value.array = (CMPIArray *) addClArray(hdr, d);
      }
    }

    else if (hdr->type == HDR_Instance &&
             od.type == CMPI_instance && (d.state & CMPI_nullValue) == 0) {
      if (d.type != CMPI_instance) {
        _SFCB_RETURN(-CMPI_RC_ERR_TYPE_MISMATCH);
      } else {
        (p + i - 1)->data = d;
        int             size = getInstanceSerializedSize(d.value.inst);
        void           *blob = malloc(size);
        getSerializedInstance(d.value.inst, blob);
        if (od.value.chars) {
          replaceClObject(hdr, (long) od.value.chars, blob, size);
        } else {
          (p + i - 1)->data.value.inst =
              (CMPIInstance *) addClObject(hdr, blob, size);
        }
        free(blob);
      }
    } 
    /* bugzilla 75997 - hdr->type is args */
    else if (hdr->type == HDR_Args &&
            od.type == CMPI_instance && (d.state & CMPI_nullValue) == 0) {
       if (d.type != CMPI_instance) {
          _SFCB_RETURN(-CMPI_RC_ERR_TYPE_MISMATCH);
       }
       else {
          (p + i - 1)->data = d;
          int size = getInstanceSerializedSize(d.value.inst);
          void * blob = malloc(size);
          getSerializedInstance(d.value.inst, blob);
          //fprintf(stderr, "od.value.inst = %ld\n", (long)od.value.inst);
          if (od.value.inst) {
             removeClObject(hdr, (long)od.value.inst);
          }
          (p + i - 1)->data.value.inst = (CMPIInstance *) addClObject(hdr, blob, size);
          free(blob);
         }
    }
    else(p + i - 1)->data = d;

    _SFCB_RETURN(i);
  }
}

static void
ClObjectRelocateStringBuffer(ClObjectHdr * hdr, ClStrBuf * buf)
{
  if (buf == NULL)
    return;
  buf = getStrBufPtr(hdr);
  buf->indexPtr = (int *) ((char *) hdr + buf->indexOffset);
  buf->iMax = GetMax(buf->iMax);
}

static void
ClObjectRelocateArrayBuffer(ClObjectHdr * hdr, ClArrayBuf * buf)
{
  if (buf == NULL)
    return;
  buf = getArrayBufPtr(hdr);
  buf->indexPtr = (int *) ((char *) hdr + buf->indexOffset);
  buf->iMax = GetMax(buf->iMax);
}

// -------------------------------------------------------
// -----
// -- Class support
// -----
// -------------------------------------------------------

static ClClass *
newClassH(ClObjectHdr * hdr, const char *cn, const char *pa)
{
  ClClass        *cls = malloc(sizeof(*cls));
  if (hdr == NULL)
    hdr = &cls->hdr;

  memset(cls, 0, sizeof(ClClass));
  hdr->type = HDR_Class;
  if (cn)
    cls->name.id = addClString(hdr, cn);
  else
    cls->name = nls;

  cls->parents = 0;
  if (pa) {
    cls->parent.id = addClString(hdr, pa);
    cls->parents = 1;
  } else
    cls->parent = nls;
  cls->quals = 0;
  cls->reserved = 0;

  clearClSection(&cls->qualifiers);
  clearClSection(&cls->properties);
  clearClSection(&cls->methods);

  return cls;
}

unsigned char
ClClassAddGrandParent(ClClass * cls, char *gp)
{
  long            id;

  id = addClString(&cls->hdr, gp);
  cls->parents++;
  return (unsigned char) id;
}

ClClass        *
ClClassNew(const char *cn, const char *pa)
{
  return newClassH(NULL, cn, pa);
}

static long
sizeClassH(ClObjectHdr * hdr, ClClass * cls)
{
  long            sz = sizeof(*cls);

  sz += sizeQualifiers(&cls->qualifiers);
  sz += sizeProperties(hdr, &cls->properties);
  sz += sizeMethods(hdr, &cls->methods);
  sz += sizeStringBuf(hdr);
  sz += sizeArrayBuf(hdr);

  return ALIGN(sz, CLALIGN);
}

unsigned long
ClSizeClass(ClClass * cls)
{
  return sizeClassH(&cls->hdr, cls);
}

static ClClass *
rebuildClassH(ClObjectHdr * hdr, ClClass * cls, void *area)
{
  int             ofs = sizeof(ClClass);
  int             sz = ClSizeClass(cls);

  sz = ALIGN(sz, CLALIGN) + CLEXTRA;
  ClClass        *nc = area ? (ClClass *) area : malloc(sz);

  *nc = *cls;

  nc->hdr.flags &= ~HDR_Rebuild;
  ofs += copyQualifiers(ofs, (char *) nc, &nc->qualifiers, hdr,
                        &cls->qualifiers);
  ofs += copyProperties(ofs, (char *) nc, &nc->properties, hdr,
                        &cls->properties);
  ofs += copyMethods(ofs, (char *) nc, &nc->methods, hdr,
                     &cls->methods);

  ofs += copyStringBuf(ofs, &nc->hdr, hdr);
  ofs += copyArrayBuf(ofs, &nc->hdr, hdr);

  nc->hdr.size = ALIGN(sz, CLALIGN);

  if (CLEXTRA)
    memcpy(((char *) nc) + sz - 4, "%%%%", 4);

  return nc;
}

ClClass        *
ClClassRebuildClass(ClClass * cls, void *area)
{
  return rebuildClassH(&cls->hdr, cls, area);
}

void
ClClassRelocateClass(ClClass * cls)
{
  _SFCB_ENTER(TRACE_OBJECTIMPL, "ClClassRelocateClass");
  ClObjectRelocateStringBuffer(&cls->hdr, cls->hdr.strBuffer);
  ClObjectRelocateArrayBuffer(&cls->hdr, cls->hdr.arrayBuffer);
  _SFCB_EXIT();
}

void
ClClassFreeClass(ClClass * cls)
{
  if (cls->hdr.flags & HDR_Rebuild) {
    int meths = ClClassGetMethodCount(cls);
    freeQualifiers(&cls->qualifiers);
    freeProperties(&cls->hdr, &cls->properties);
    freeMethods(&cls->hdr, &cls->methods, meths);
    freeStringBuf(&cls->hdr);
    freeArrayBuf(&cls->hdr);
  }
  free(cls);
}

char           *
ClClassToString(ClClass * cls)
{
  stringControl   sc = { NULL, 0, 32 };
  ClProperty     *p;
  int             sb = 2,
      i,
      l;
  ClQualifier    *q;
  unsigned long   quals;

  q = (ClQualifier *) ClObjectGetClSection(&cls->hdr, &cls->qualifiers);
  quals = cls->quals;
  if ((l = cls->qualifiers.used)) {
    for (i = 0; i < l; sb = 0, i++) {
      if ((quals == 0) && (i == l - 1))
        sb |= 1;
      addQualifierToString(&sc, &cls->hdr, q + i, sb);
    }
    if (quals) {
      cat2string(&sc, "\n   ");
      if (quals & ClClass_Q_Abstract)
        cat2string(&sc, ",Abstract");
      if (quals & ClClass_Q_Association)
        cat2string(&sc, ",Association");
      if (quals & ClClass_Q_Indication)
        cat2string(&sc, ",Indication");
      cat2string(&sc, "]");
    }
    cat2string(&sc, "\n");
  }

  cat2string(&sc, "class ");
  cat2string(&sc, ClObjectGetClString(&cls->hdr, &cls->name));
  if (cls->parent.id) {
    cat2string(&sc, ":");
    cat2string(&sc, ClObjectGetClString(&cls->hdr, &cls->parent));
  }
  cat2string(&sc, " {\n");

  p = (ClProperty *) ClObjectGetClSection(&cls->hdr, &cls->properties);
  for (i = 0, l = cls->properties.used; i < l; i++) {
    addPropertyToString(&sc, &cls->hdr, p + i);
  }

  cat2string(&sc, "};\n");

  return sc.str;
}

// 
// --- Class Properties
// 

int
ClClassAddProperty(ClClass * cls, const char *id, CMPIData d,
                   char *refName)
{
  ClSection      *prps = &cls->properties;
  return addObjectPropertyH(&cls->hdr, prps, id, d, refName);
}

int
ClClassGetPropertyCount(ClClass * cls)
{
  return cls->properties.used;
}

int
ClClassGetPropertyAt(ClClass * cls, int id, CMPIData *data, char **name,
                     unsigned long *quals, char **refName)
{
  char           *rName;
  ClProperty     *p;
  p = (ClProperty *) ClObjectGetClSection(&cls->hdr, &cls->properties);
  if (id < 0 || id > cls->properties.used)
    return 1;

  if ((p + id)->quals & ClProperty_Q_EmbeddedObject) {
    data->type =
        (data->type & CMPI_ARRAY ? CMPI_instance | CMPI_ARRAY :
         CMPI_instance);
  }

  if (data) {
    *data = (p + id)->data;
    if (data->state & CMPI_nullValue) {
      data->value.uint64 = 0;
    } else if (data->type == CMPI_chars) {
      const char     *str =
          ClObjectGetClString(&cls->hdr, (ClString *) & data->value.chars);
      data->value.string = sfcb_native_new_CMPIString(str, NULL, 0);
      data->type = CMPI_string;
    } else if (data->type == CMPI_dateTime) {
      const char     *str =
          ClObjectGetClString(&cls->hdr, (ClString *) & data->value.chars);
      data->value.dateTime =
          sfcb_native_new_CMPIDateTime_fromChars(str, NULL);
    } else if (data->type & CMPI_ARRAY) {
      data->value.dataPtr.ptr = (void *) ClObjectGetClArray(&cls->hdr,
                                                            (ClArray *) &
                                                            data->value.
                                                            array);
    } else if (data->type == CMPI_instance) {
      data->value.inst = relocateSerializedInstance((void *)
                                                    ClObjectGetClObject
                                                    (&cls->hdr,
                                                     (ClString *) &
                                                     data->value.inst));
    }
  }

  if (name)
    *name = (char *) ClObjectGetClString(&cls->hdr, &(p + id)->id);
  if (quals)
    *quals = (p + id)->quals;
  if (refName) {
    rName = (char *) ClObjectGetClString(&cls->hdr, &(p + id)->refName);
    if (rName) {
      *refName = rName;
    } else {
      *refName = NULL;
    }
  }

  return 0;
}

int
ClClassGetPropQualifierCount(ClClass * cls, int id)
{
  ClProperty     *p;

  p = (ClProperty *) ClObjectGetClSection(&cls->hdr, &cls->properties);
  if (id < 0 || id > cls->properties.used)
    return -1;
  return (p + id)->qualifiers.used;
}

int
ClClassGetPropQualifierAt(ClClass * cls, int id, int qid, CMPIData *data,
                          char **name)
{
  ClProperty     *p;
  ClQualifier    *q;

  p = (ClProperty *) ClObjectGetClSection(&cls->hdr, &cls->properties);
  if (id < 0 || id > cls->properties.used)
    return 1;
  p = p + id;

  q = (ClQualifier *) ClObjectGetClSection(&cls->hdr, &p->qualifiers);
  if (qid < 0 || qid > p->qualifiers.used)
    return 1;
  return ClGetQualifierAt(cls, q, qid, data, name);
}

// 
// --- Class Methods
// 

int
ClClassAddMethod(ClClass * cls, const char *id, CMPIType t)
{
  ClSection      *mths = &cls->methods;
  return addClassMethodH(&cls->hdr, mths, id, t);
}

int
ClClassGetMethodCount(ClClass * cls)
{
  return cls->methods.used;
}

int
ClClassGetMethodAt(ClClass * cls, int id, CMPIType *data, char **name,
                   unsigned long *quals)
{
  ClMethod       *m;
  m = (ClMethod *) ClObjectGetClSection(&cls->hdr, &cls->methods);
  if (id < 0 || id > cls->methods.used)
    return 1;
  if (data)
    *data = (m + id)->type;
  if (name)
    *name = (char *) ClObjectGetClString(&cls->hdr, &(m + id)->id);
  if (quals)
    *quals = (m + id)->quals;
  return 0;
}

int
ClClassGetMethQualifierAt(ClClass * cls, ClMethod * m, int qid,
                          CMPIData *data, char **name)
{
  ClQualifier    *q;

  q = (ClQualifier *) ClObjectGetClSection(&cls->hdr, &m->qualifiers);
  if (qid < 0 || qid > m->qualifiers.used)
    return 1;

  ClGetQualifierAt(cls, q, qid, data, name);
  if (data->type & CMPI_ARRAY && data->value.array) {
    data->value.array =
        native_make_CMPIArray((CMPIData *) data->value.array, NULL,
                              &cls->hdr);
  }

  return 0;
}

int
ClClassGetMethParameterAt(ClClass * cls, ClMethod * m, int pid,
                          CMPIParameter * parm, char **name)
{
  ClParameter    *p;

  p = (ClParameter *) ClObjectGetClSection(&cls->hdr, &m->parameters);
  if (pid < 0 || pid > m->parameters.used)
    return 1;
  if (parm) {
    *parm = (p + pid)->parameter;
    if (parm->refName) {
      parm->refName =
          (char *) ClObjectGetClString(&cls->hdr,
                                       (ClString *) & parm->refName);
    }
  }
  if (name)
    *name = (char *) ClObjectGetClString(&cls->hdr, &(p + pid)->id);
  return 0;
}

//This function checks whether Methods from the class are filtered or not
int ClClassIsMethodAtFiltered(ClClass * inst, int id)
{
  ClMethod *p;
  p = (ClMethod *) ClObjectGetClSection(&inst->hdr, &inst->methods);
  if((p + id)->flags & ClProperty_Filtered) {
    return 1;
  }
  else {
    return 0;
  }
}

//This function checks whether Properties from the class are filtered or not
int ClClassIsPropertyAtFiltered(ClClass * inst, int id)
{
  ClProperty *p;
  p = (ClProperty *) ClObjectGetClSection(&inst->hdr, &inst->properties);
  if((p + id)->flags & ClProperty_Filtered) {
    return 1;
  }
  else {
    return 0;
  }
}

void ClClassSetHasFilteredProps(ClClass *cls) {
  cls->hdr.flags |= HDR_HasFilteredProps;
}

int ClClassHasFilteredProps(ClClass *cls) {
  return ((cls->hdr.flags & HDR_HasFilteredProps) > 0);
}

// -------------------------------------------------------
// -----
// -- Instance support
// -----
// -------------------------------------------------------

int
isInstance(const CMPIInstance *ci)
{
  ClInstance     *inst = (ClInstance *) ci->hdl;
  if (inst->hdr.type == HDR_Instance)
    return 1;
  return 0;
}

static ClInstance *
newInstanceH(const char *ns, const char *cn)
{
  ClInstance     *inst = malloc(sizeof(*inst));

  memset(inst, 0, sizeof(ClInstance));
  inst->hdr.type = HDR_Instance;
  if (ns)
    inst->nameSpace.id = addClString(&inst->hdr, ns);
  else
    inst->nameSpace = nls;
  if (cn)
    inst->className.id = addClString(&inst->hdr, cn);
  else
    inst->className = nls;
  inst->quals = 0;
  inst->parents = 0;
  inst->reserved = 0;
  inst->path = NULL;

  clearClSection(&inst->qualifiers);
  clearClSection(&inst->properties);

  return inst;
}

ClInstance     *
ClInstanceNew(const char *ns, const char *cn)
{
  return newInstanceH(ns, cn);
}

ClInstance     *
ClInstanceNewFromMof(const char *ns, const char *cn)
{
  ClInstance     *inst = newInstanceH(ns, cn);
  if (inst)
    inst->hdr.flags |= HDR_FromMof;
  return inst;
}

static long
sizeInstanceH(ClObjectHdr * hdr, ClInstance * inst)
{
  long            sz = sizeof(*inst);

  sz += sizeQualifiers(&inst->qualifiers);
  sz += sizeProperties(hdr, &inst->properties);
  sz += sizeStringBuf(hdr);
  sz += sizeArrayBuf(hdr);

  return ALIGN(sz, CLALIGN);
}

unsigned long
ClSizeInstance(ClInstance * inst)
{
  return sizeInstanceH(&inst->hdr, inst);
}

static ClInstance *
rebuildInstanceH(ClObjectHdr * hdr, ClInstance * inst, void *area)
{
  int             ofs = sizeof(ClInstance);
  int             sz = ClSizeInstance(inst);
  ClInstance     *ni = area ? (ClInstance *) area : malloc(sz);

  *ni = *inst;
  ni->hdr.flags &= ~HDR_Rebuild;
  ofs += copyQualifiers(ofs, (char *) ni, &ni->qualifiers, hdr,
                        &inst->qualifiers);
  ofs += copyProperties(ofs, (char *) ni, &ni->properties, hdr,
                        &inst->properties);
  ofs += copyStringBuf(ofs, &ni->hdr, hdr);
  ofs += copyArrayBuf(ofs, &ni->hdr, hdr);

  ni->hdr.size = ALIGN(sz, CLALIGN);

  return ni;
}

ClInstance     *
ClInstanceRebuild(ClInstance * inst, void *area)
{
  return rebuildInstanceH(&inst->hdr, inst, area);
}

void
ClInstanceRelocateInstance(ClInstance * inst)
{
  _SFCB_ENTER(TRACE_OBJECTIMPL, "ClInstanceRelocateInstance");
  ClObjectRelocateStringBuffer(&inst->hdr, inst->hdr.strBuffer);
  ClObjectRelocateArrayBuffer(&inst->hdr, inst->hdr.arrayBuffer);
  _SFCB_EXIT();
}

void
ClInstanceFree(ClInstance * inst)
{
  if (inst->hdr.flags & HDR_Rebuild) {
    freeQualifiers( &inst->qualifiers);
    freeProperties(&inst->hdr, &inst->properties);
    freeStringBuf(&inst->hdr);
    freeArrayBuf(&inst->hdr);
  }
  free(inst);
}

char           *
ClInstanceToString(ClInstance * inst)
{
  stringControl   sc = { NULL, 0, 32 };
  ClProperty     *p;
  int             sb = 2,
      i,
      l;
  ClQualifier    *q;

  q = (ClQualifier *) ClObjectGetClSection(&inst->hdr, &inst->qualifiers);
  if ((l = inst->qualifiers.used)) {
    for (i = 0; i < l; sb = 0, i++) {
      if (i == l - 1)
        sb |= 1;
      addQualifierToString(&sc, &inst->hdr, q + i, sb);
    }
    cat2string(&sc, "\n");
  }

  cat2string(&sc, "Instance of ");
  cat2string(&sc, ClObjectGetClString(&inst->hdr, &inst->className));
  cat2string(&sc, " {\n");

  p = (ClProperty *) ClObjectGetClSection(&inst->hdr, &inst->properties);
  for (i = 0, l = inst->properties.used; i < l; i++) {
    addPropertyToString(&sc, &inst->hdr, (p + i));
  }

  cat2string(&sc, "};\n");

  return sc.str;
}

int
ClInstanceGetPropertyCount(ClInstance * inst)
{
  ClObjectGetClSection(&inst->hdr, &inst->properties);
  return inst->properties.used;
}

int
ClInstanceGetPropertyAt(ClInstance * inst, int id, CMPIData *data,
                        char **name, unsigned long *quals)
{
  ClProperty     *p;
  _SFCB_ENTER(TRACE_OBJECTIMPL, "ClInstanceGetPropertyAt");

  p = (ClProperty *) ClObjectGetClSection(&inst->hdr, &inst->properties);
  if (id < 0 || id > inst->properties.used)
    _SFCB_RETURN(1);
  if (data) {
    *data = (p + id)->data;
    if (data->type == CMPI_chars) {
      const char     *str = ClObjectGetClString(&inst->hdr,
                                                (ClString *) & data->
                                                value.chars);
      data->value.string = sfcb_native_new_CMPIString(str, NULL, 2);
      data->type = CMPI_string;
    }
    if (data->type == CMPI_dateTime) {
      const char     *str = ClObjectGetClString(&inst->hdr,
                                                (ClString *) & data->
                                                value.chars);
      data->value.dateTime =
          sfcb_native_new_CMPIDateTime_fromChars(str, NULL);
    }
    if (data->type & CMPI_ARRAY) {
      data->value.dataPtr.ptr = (void *) ClObjectGetClArray(&inst->hdr,
                                                            (ClArray *) &
                                                            data->value.
                                                            array);
    }
    if (data->type == CMPI_instance) {
      data->value.inst = (void *) ClObjectGetClObject(&inst->hdr,
                                                      (ClString *) &
                                                      data->value.inst);
      if (data->value.inst) {
        relocateSerializedInstance(data->value.inst);
      }
    }
  }

  if (name)
    *name = (char *) ClObjectGetClString(&inst->hdr, &(p + id)->id);
  if (quals)
    *quals = (p + id)->quals;

  _SFCB_RETURN(0);
}

int ClInstanceAddPropertyQualifierSpecial(ClInstance *inst, const char *id,
                                          const char *qualifier)
{
  int i;
  ClProperty *p;

  _SFCB_ENTER(TRACE_OBJECTIMPL, "ClInstanceAddPropertyQualifierSpecial");

  i = ClObjectLocateProperty(&inst->hdr, &inst->properties, id);
  if (i == 0)
    /* cannot find the property */
    _SFCB_RETURN(CMPI_RC_ERR_NO_SUCH_PROPERTY);

  p = (ClProperty*) getSectionPtr(&inst->hdr, &inst->properties);
  p = p + i - 1;

  if (strcasecmp(qualifier, "embeddedinstance") == 0) {
    p->quals |= ClProperty_Q_EmbeddedObject;
    p->quals |= ClProperty_Q_EmbeddedInstance;
  } else if (strcasecmp(qualifier, "embeddedobject") == 0) {
    p->quals |= ClProperty_Q_EmbeddedObject;
  }

  _SFCB_RETURN(0)
}


int
ClInstanceAddProperty(ClInstance * inst, const char *id, CMPIData d)
{
  ClSection      *prps = &inst->properties;
  return addObjectPropertyH(&inst->hdr, prps, id, d, NULL);
}

int
ClInstanceIsPropertyAtFiltered(ClInstance * inst, int id)
{
  ClProperty     *p;
  p = (ClProperty *) ClObjectGetClSection(&inst->hdr, &inst->properties);
  if ((p + id)->flags & ClProperty_Filtered) {
    return 1;
  } else {
    return 0;
  }
}

void
ClInstanceFilterFlagProperty(ClInstance * inst, int id)
{
  ClProperty     *p;
  p = (ClProperty *) ClObjectGetClSection(&inst->hdr, &inst->properties);
  (p + id)->flags |= ClProperty_Filtered;
}

const char     *
ClInstanceGetClassName(ClInstance * inst)
{
  return ClObjectGetClString(&inst->hdr, &inst->className);
}

const char     *
ClInstanceGetNameSpace(ClInstance * inst)
{
  return ClObjectGetClString(&inst->hdr, &inst->nameSpace);
}

const char     *
ClGetStringData(CMPIInstance *ci, int id)
{
  ClInstance     *inst = (ClInstance *) ci->hdl;
  ClString        sid = { id };
  return ClObjectGetClString(&inst->hdr, &sid);
}

// -------------------------------------------------------
// -----
// -- ObjectPath support
// -----
// -------------------------------------------------------

static ClObjectPath *
newObjectPathH(const char *ns, const char *cn)
{
  _SFCB_ENTER(TRACE_OBJECTIMPL, "newObjectPathH");

  ClObjectPath   *op = malloc(sizeof(*op));

  memset(op, 0, sizeof(ClObjectPath));
  op->hdr.type = HDR_ObjectPath;
  if (ns)
    op->nameSpace.id = addClString(&op->hdr, ns);
  else
    op->nameSpace = nls;
  if (cn)
    op->className.id = addClString(&op->hdr, cn);
  else
    op->className = nls;

  op->hostName = nls;

  clearClSection(&op->properties);

  _SFCB_RETURN(op);
}

ClObjectPath   *
ClObjectPathNew(const char *ns, const char *cn)
{
  return newObjectPathH(ns, cn);
}

static long
sizeObjectPathH(ClObjectHdr * hdr, ClObjectPath * op)
{
  long            sz = sizeof(*op);

  _SFCB_ENTER(TRACE_OBJECTIMPL, "sizeObjectPathH");

  sz += sizeProperties(hdr, &op->properties);
  sz += sizeStringBuf(hdr);

  _SFCB_RETURN(ALIGN(sz, CLALIGN));
}

unsigned long
ClSizeObjectPath(ClObjectPath * op)
{
  return sizeObjectPathH(&op->hdr, op);
}

static ClObjectPath *
rebuildObjectPathH(ClObjectHdr * hdr, ClObjectPath * op, void *area)
{
  _SFCB_ENTER(TRACE_OBJECTIMPL, "rebuildObjectPathH");

  int             ofs = sizeof(ClObjectPath);
  int             sz = ClSizeObjectPath(op);
  ClObjectPath   *nop = area ? (ClObjectPath *) area : malloc(sz);

  *nop = *op;
  nop->hdr.flags &= ~HDR_Rebuild;
  ofs += copyProperties(ofs, (char *) nop, &nop->properties, hdr,
                        &op->properties);
  ofs += copyStringBuf(ofs, &nop->hdr, hdr);
  nop->hdr.size = ALIGN(sz, CLALIGN);

  _SFCB_RETURN(nop);
}

ClObjectPath   *
ClObjectPathRebuild(ClObjectPath * op, void *area)
{
  return rebuildObjectPathH(&op->hdr, op, area);
}

void
ClObjectPathRelocateObjectPath(ClObjectPath * op)
{
  _SFCB_ENTER(TRACE_OBJECTIMPL, "ClObjectPathRelocateObjectPath");
  ClObjectRelocateStringBuffer(&op->hdr, op->hdr.strBuffer);
  ClObjectRelocateArrayBuffer(&op->hdr, op->hdr.arrayBuffer);
  _SFCB_EXIT();
}

void
ClObjectPathFree(ClObjectPath * op)
{
  if (op->hdr.flags & HDR_Rebuild) {
    freeProperties(&op->hdr, &op->properties);
    freeStringBuf(&op->hdr);
  }
  free(op);
}

char           *
ClObjectPathToString(ClObjectPath __attribute__ ((unused)) *op)
{
  stringControl   sc = { NULL, 0, 32 };
  // ClProperty *p;
  // int sb = 2, i, l;

  cat2string(&sc, "***ObjectPath not done yet***");
  /*
   * cat2string(&sc, ClObjectGetClString(&op->hdr, &op->name));
   * cat2string(&sc, " {\n");
   * 
   * p = (ClProperty *) ClObjectGetClSection(&op->hdr, &op->properties);
   * for (i = 0, l = op->properties.used; i < l; i++) {
   * addPropertyToString(&sc, &op->hdr, p + i); }
   * 
   * cat2string(&sc, "};\n"); 
   */
  return sc.str;
}

int
ClObjectPathGetKeyCount(ClObjectPath * op)
{
  ClObjectGetClSection(&op->hdr, &op->properties);
  return op->properties.used;
}

int
ClObjectPathGetKeyAt(ClObjectPath * op, int id, CMPIData *data,
                     char **name)
{
  ClProperty     *p;

  p = (ClProperty *) ClObjectGetClSection(&op->hdr, &op->properties);
  if (id < 0 || id > op->properties.used)
    return 1;
  if (data) {
    *data = (p + id)->data;
    if (data->type == CMPI_chars) {
      const char     *str =
          ClObjectGetClString(&op->hdr, (ClString *) & data->value.chars);
      data->value.string = sfcb_native_new_CMPIString(str, NULL, 0);
      data->type = CMPI_string;
    } else if (data->type == CMPI_dateTime) {
      const char     *str =
          ClObjectGetClString(&op->hdr, (ClString *) & data->value.chars);
      data->value.dateTime =
          sfcb_native_new_CMPIDateTime_fromChars(str, NULL);
    }
  }
  if (name)
    *name = (char *) ClObjectGetClString(&op->hdr, &(p + id)->id);

  return 0;
}

int
ClObjectPathAddKey(ClObjectPath * op, const char *id, CMPIData d)
{
  _SFCB_ENTER(TRACE_OBJECTIMPL, "ClObjectPathAddKey");

  ClSection      *prps = &op->properties;
  _SFCB_RETURN(addObjectPropertyH(&op->hdr, prps, id, d, NULL));
}

void
ClObjectPathSetHostName(ClObjectPath * op, const char *hn)
{
  if (op->hostName.id)
    replaceClString(&op->hdr, op->hostName.id, hn);
  else if (hn)
    op->hostName.id = addClString(&op->hdr, hn);
  else
    op->hostName = nls;
}

const char     *
ClObjectPathGetHostName(ClObjectPath * op)
{
  return ClObjectGetClString(&op->hdr, &op->hostName);
}

void
ClObjectPathSetNameSpace(ClObjectPath * op, const char *ns)
{
  if (op->nameSpace.id)
    replaceClString(&op->hdr, op->nameSpace.id, ns);
  else if (ns)
    op->nameSpace.id = addClString(&op->hdr, ns);
  else
    op->nameSpace = nls;
}

const char     *
ClObjectPathGetNameSpace(ClObjectPath * op)
{
  return ClObjectGetClString(&op->hdr, &op->nameSpace);
}

void
ClObjectPathSetClassName(ClObjectPath * op, const char *cn)
{
  if (op->className.id)
    replaceClString(&op->hdr, op->className.id, cn);
  else if (cn)
    op->className.id = addClString(&op->hdr, cn);
  else
    op->className = nls;
}

const char     *
ClObjectPathGetClassName(ClObjectPath * op)
{
  return ClObjectGetClString(&op->hdr, &op->className);
}

// -------------------------------------------------------
// -----
// -- Args support
// -----
// -------------------------------------------------------

static ClArgs  *
newArgsH()
{
  _SFCB_ENTER(TRACE_OBJECTIMPL, "newArgsH");
  ClArgs         *arg = malloc(sizeof(*arg));

  memset(arg, 0, sizeof(ClArgs));
  arg->hdr.type = HDR_Args;
  clearClSection(&arg->properties);

  _SFCB_RETURN(arg);
}

ClArgs         *
ClArgsNew()
{
  return newArgsH();
}

static long
sizeArgsH(ClObjectHdr * hdr, ClArgs * arg)
{
  _SFCB_ENTER(TRACE_OBJECTIMPL, "sizeArgsH");
  long            sz = sizeof(*arg);

  sz += sizeProperties(hdr, &arg->properties);
  sz += sizeStringBuf(hdr);
  sz += sizeArrayBuf(hdr);

  _SFCB_RETURN(ALIGN(sz, CLALIGN));
}

unsigned long
ClSizeArgs(ClArgs * arg)
{
  return sizeArgsH(&arg->hdr, arg);
}

static ClArgs  *
rebuildArgsH(ClObjectHdr * hdr, ClArgs * arg, void *area)
{
  _SFCB_ENTER(TRACE_OBJECTIMPL, "rebuildArgsH");
  int             ofs = sizeof(ClArgs);
  int             sz = ClSizeArgs(arg);
  ClArgs         *nop = area ? (ClArgs *) area : malloc(sz);

  *nop = *arg;
  nop->hdr.flags &= ~HDR_Rebuild;
  ofs += copyProperties(ofs, (char *) nop, &nop->properties, hdr,
                        &arg->properties);
  ofs += copyStringBuf(ofs, &nop->hdr, hdr);
  ofs += copyArrayBuf(ofs, &nop->hdr, hdr);
  nop->hdr.size = ALIGN(sz, CLALIGN);

  _SFCB_RETURN(nop);
}

ClArgs         *
ClArgsRebuild(ClArgs * arg, void *area)
{
  return rebuildArgsH(&arg->hdr, arg, area);
}

void
ClArgsRelocateArgs(ClArgs * arg)
{
  _SFCB_ENTER(TRACE_OBJECTIMPL, "ClArgsRelocateArgs");

  ClObjectRelocateStringBuffer(&arg->hdr, arg->hdr.strBuffer);
  ClObjectRelocateArrayBuffer(&arg->hdr, arg->hdr.arrayBuffer);
  _SFCB_EXIT();
}

void
ClArgsFree(ClArgs * arg)
{
  _SFCB_ENTER(TRACE_OBJECTIMPL, "ClArgsFree");
  if (arg->hdr.flags & HDR_Rebuild) {
    freeProperties(&arg->hdr, &arg->properties);
    freeStringBuf(&arg->hdr);
    freeArrayBuf(&arg->hdr);
  }
  free(arg);
  _SFCB_EXIT();
}

int
ClArgsGetArgCount(ClArgs * arg)
{
  _SFCB_ENTER(TRACE_OBJECTIMPL, "ClArgsGetArgCount");
  ClObjectGetClSection(&arg->hdr, &arg->properties);
  _SFCB_RETURN(arg->properties.used);
}

int
ClArgsGetArgAt(ClArgs * arg, int id, CMPIData *data, char **name)
{
  ClProperty     *p;
  _SFCB_ENTER(TRACE_OBJECTIMPL, "ClArgsGetArgAt");

  p = (ClProperty *) ClObjectGetClSection(&arg->hdr, &arg->properties);
  if (id < 0 || id > arg->properties.used)
    return 1;
  if (data) {
    *data = (p + id)->data;
    if (data->type == CMPI_chars) {
      const char     *str =
          ClObjectGetClString(&arg->hdr, (ClString *) & data->value.chars);
      data->value.string = sfcb_native_new_CMPIString(str, NULL, 0);
      data->type = CMPI_string;
    }
    if (data->type == CMPI_dateTime) {
      const char     *str =
          ClObjectGetClString(&arg->hdr, (ClString *) & data->value.chars);
      data->value.dateTime =
          sfcb_native_new_CMPIDateTime_fromChars(str, NULL);
    }
    if (data->type & CMPI_ARRAY) {
      data->value.dataPtr.ptr = (void *) ClObjectGetClArray(&arg->hdr,
                                                            (ClArray *) &
                                                            data->value.
                                                            array);
    }
    if (data->type == CMPI_instance) {
      data->value.inst = (void *) ClObjectGetClObject(&arg->hdr,
                                                      (ClString *) &
                                                      data->value.inst);
      if (data->value.inst) {
        relocateSerializedInstance(data->value.inst);
      }
    }
  }
  if (name)
    *name = (char *) ClObjectGetClString(&arg->hdr, &(p + id)->id);

  _SFCB_RETURN(0);
}

int
ClArgsAddArg(ClArgs * arg, const char *id, CMPIData d)
{
  _SFCB_ENTER(TRACE_OBJECTIMPL, "ClArgsAddArg");
  ClSection      *prps = &arg->properties;
  _SFCB_RETURN(addObjectPropertyH(&arg->hdr, prps, id, d, NULL));
}

// -------------------------------------------------------
// -----
// -- Qualifier support
// -----
// -------------------------------------------------------

static ClQualifierDeclaration *
newQualifierDeclarationH(ClObjectHdr * hdr, const char *ns,
                         const char *name)
{
  ClQualifierDeclaration *q = malloc(sizeof(*q));
  if (hdr == NULL)
    hdr = &q->hdr;

  memset(q, 0, sizeof(ClQualifierDeclaration));
  hdr->type = HDR_Qualifier;
  if (name)
    q->qualifierName.id = addClString(hdr, name);
  else
    q->qualifierName = nls;

  if (ns)
    q->nameSpace.id = addClString(&q->hdr, ns);
  else
    q->nameSpace = nls;

  q->flavor = 0;
  q->scope = 0;
  q->type = 0;
  q->arraySize = 0;

  clearClSection(&q->qualifierData);

  return q;
}

ClQualifierDeclaration *
ClQualifierDeclarationNew(const char *ns, const char *name)
{
  return newQualifierDeclarationH(NULL, ns, name);
}

static long
sizeQualifierDeclarationH(ClObjectHdr * hdr, ClQualifierDeclaration * q)
{
  long            sz = sizeof(*q);

  sz += sizeQualifiers(&q->qualifierData);
  sz += sizeStringBuf(hdr);
  sz += sizeArrayBuf(hdr);

  return ALIGN(sz, CLALIGN);
}

unsigned long
ClSizeQualifierDeclaration(ClQualifierDeclaration * q)
{
  return sizeQualifierDeclarationH(&q->hdr, q);
}

static ClQualifierDeclaration *
rebuildQualifierH(ClObjectHdr * hdr, ClQualifierDeclaration * q,
                  void *area)
{
  int             ofs = sizeof(ClQualifierDeclaration);
  int             sz = ClSizeQualifierDeclaration(q);

  sz = ALIGN(sz, CLALIGN) + CLEXTRA;
  ClQualifierDeclaration *nq =
      area ? (ClQualifierDeclaration *) area : (ClQualifierDeclaration *)
      malloc(sz);

  *nq = *q;

  nq->hdr.flags &= ~HDR_Rebuild;
  ofs +=
      copyQualifiers(ofs, (char *) nq, &nq->qualifierData, hdr,
                     &q->qualifierData);
  ofs += copyStringBuf(ofs, &nq->hdr, hdr);
  ofs += copyArrayBuf(ofs, &nq->hdr, hdr);

  nq->hdr.size = ALIGN(sz, CLALIGN);

  if (CLEXTRA)
    memcpy(((char *) nq) + sz - 4, "%%%%", 4);

  return nq;
}

ClQualifierDeclaration *
ClQualifierRebuildQualifier(ClQualifierDeclaration * q, void *area)
{
  return rebuildQualifierH(&q->hdr, q, area);
}

void
ClQualifierRelocateQualifier(ClQualifierDeclaration * q)
{
  _SFCB_ENTER(TRACE_OBJECTIMPL, "ClQualifierRelocateQualifier");
  ClObjectRelocateStringBuffer(&q->hdr, q->hdr.strBuffer);
  ClObjectRelocateArrayBuffer(&q->hdr, q->hdr.arrayBuffer);
  _SFCB_EXIT();
}

int
ClQualifierAddQualifier(ClObjectHdr * hdr, ClSection * qlfs,
                        const char *id, CMPIData d)
{
  return addClQualifier(hdr, qlfs, id, d, 0);
}

static int
ClGetQualifierFromQualifierDeclaration(ClQualifierDeclaration * q,
                                       ClQualifier * qData, CMPIData *data)
{
  if (data) {
    *data = qData->data;
    if (data->type == CMPI_chars) {
      const char     *str =
          ClObjectGetClString(&q->hdr, (ClString *) & data->value.chars);
      data->value.string = sfcb_native_new_CMPIString(str, NULL, 0);
      data->type = CMPI_string;
    } else if (data->type & CMPI_ARRAY) {
      data->value.dataPtr.ptr = (void *) ClObjectGetClArray(&q->hdr,
                                                            (ClArray *) &
                                                            data->value.
                                                            array);
    }
  }
  return 0;
}

int
ClQualifierDeclarationGetQualifierData(ClQualifierDeclaration * q,
                                       CMPIData *data)
{
  ClQualifier    *qData;
  qData = (ClQualifier *) ClObjectGetClSection(&q->hdr, &q->qualifierData);
  return ClGetQualifierFromQualifierDeclaration(q, qData, data);
}

void
ClQualifierFree(ClQualifierDeclaration * q)
{
  if (q->hdr.flags & HDR_Rebuild) {
    freeQualifiers(&q->qualifierData);
    freeStringBuf(&q->hdr);
    freeArrayBuf(&q->hdr);
  }
  free(q);
}

void
dumpClass(char *msg, CMPIConstClass * cls)
{
  ClClass        *cl = (ClClass *) cls->hdl;
  char           *buf = (char *) cl;
  printf("classDump: %s\n", msg);
  printf("strBuf: %p arrayBuf %p\n", buf + cl->hdr.strBufOffset,
         buf + cl->hdr.arrayBufOffset);
  dump("strBuf", buf + cl->hdr.strBufOffset, sizeof(ClStrBuf));
  dump("arrayBuf", buf + cl->hdr.arrayBufOffset, sizeof(ClArrayBuf));
  dump(msg, buf, cl->hdr.size);
}

char           *
ClArgsToString(ClArgs * arg)
{
  stringControl   sc = { NULL, 0, 32 };
  ClProperty     *p;
  int             i,
                  l;

  cat2string(&sc, "CMPIArgs ");
  cat2string(&sc, " (\n");

  p = (ClProperty *) ClObjectGetClSection(&arg->hdr, &arg->properties);
  for (i = 0, l = arg->properties.used; i < l; i++) {
    addPropertyToString(&sc, &arg->hdr, p + i);
  }

  cat2string(&sc, ");\n");

  return sc.str;
}

#ifdef UNITTEST
// Embedded unittest routine
int
oi_test()
{
  int           i,fail = 0;
  const char          *str;
  CMPIStatus    rc;

  // Some CMPI based tests
  CMPIObjectPath *cop = NewCMPIObjectPath("root", "myClass", &rc);
  if (rc.rc != CMPI_RC_OK) {
    printf("Failed to create object path, rc: %d msg:%s\n",rc.rc,(char *)rc.msg);
    fail = 1;
  }
  CMPIInstance *inst = NewCMPIInstance(cop, &rc);
  if (rc.rc != CMPI_RC_OK) {
    printf("Failed to create instance, rc: %d msg:%s\n",rc.rc,(char *)rc.msg);
    fail = 1;
  }
  i=isInstance(inst);
  if (!i) {
    printf("Failed isInstance check, rc=%d\n",i);
    fail = 1;
  }
  i=isInstance((CMPIInstance *)cop);
  if (i) {
    printf("Failed negative isInstance check, rc=%d\n",i);
    fail = 1;
  }

  //Do some array checks
  CMPIArray *ar = NewCMPIArray(1, CMPI_sint32, &rc);
  if (rc.rc != CMPI_RC_OK) {
    printf("Failed to create CMPI array, rc: %d msg:%s\n",rc.rc,(char *)rc.msg);
    fail = 1;
  }
  int val = 37;
  CMSetArrayElementAt(ar, 0, &val, CMPI_sint32);
  CMPIData ad = CMGetArrayElementAt(ar, 0, &rc);
  if (rc.rc != CMPI_RC_OK) {
    printf("Failed to get CMPI array element, rc: %d msg:%s\n",rc.rc,(char *)rc.msg);
    fail = 1;
  }
  if (ad.value.sint32 != val) {
    printf("Fetched array element value not as expected, expected: %d got:%d\n",val,ad.value.sint32);
    fail = 1;
  }
  CMSetProperty(inst, "test", &ar, CMPI_sint32A);
  CMPIData d = CMGetProperty(inst, "test", &rc);
  if (rc.rc != CMPI_RC_OK) {
    printf("Failed to get CMPI property, rc: %d msg:%s\n",rc.rc,(char *)rc.msg);
    fail = 1;
  }
  ad = CMGetArrayElementAt(d.value.array, 0, &rc);
  if (rc.rc != CMPI_RC_OK) {
    printf("Failed to get CMPI array element, rc: %d msg:%s\n",rc.rc,(char *)rc.msg);
    fail = 1;
  }
  if (ad.value.sint32 != val) {
    printf("Instance array element value not as expected, expected: %d got:%d\n",val,ad.value.sint32);
    fail = 1;
  }

  //CLobject tests

  //CLObjectPath tests
  ClObjectPath *clop=ClObjectPathNew("root","myClass");
  if (clop == NULL) {
    printf("Failed to create CLObjectPath\n");
    fail = 1;
  }
  ClObjectPath *clop2=ClObjectPathRebuild(clop,NULL);
  if (clop2 == NULL) {
    printf("Failed to rebuild CLObjectPath\n");
    fail = 1;
  }
  str=ClObjectPathToString(clop);
  // Not implemented, so not checked

  //Set it twice to test replace string
  ClObjectPathSetHostName(clop,"testhost2");
  ClObjectPathSetHostName(clop,"testhost");
  str=ClObjectPathGetHostName(clop);
  if (strcmp(str,"testhost") != 0) {
    printf("Failed to get/set hostname from objectpath:%s\n",str);
    fail = 1;
  }
  ClObjectPathSetNameSpace(clop, "root2");
  str=ClObjectPathGetNameSpace(clop);
  if (strcmp(str,"root2") != 0) {
    printf("Failed to get/set namespace from objectpath:%s\n",str);
    fail = 1;
  }
  ClObjectPathSetClassName(clop, "myClass2");
  str=ClObjectPathGetClassName(clop);
  if (strcmp(str,"myClass2") != 0) {
    printf("Failed to get/set classname from objectpath:%s\n",str);
    fail = 1;
  }

  // CLinstance checks
  ClInstance * cli=ClInstanceNewFromMof("root", "myClass");
  if (cli == NULL) {
    printf("Failed to create CLinstance from MOF\n");
    fail = 1;
  }
  ClInstance *cli2=ClInstanceRebuild(cli,NULL);
  if (cli2 == NULL) {
    printf("Failed to rebuild CLinstance\n");
    fail = 1;
  }

  unsigned long li=ClSizeInstance(cli);
  if (li == 0) {
    printf("Failed to size CLinstance.\n");
    fail = 1;
  }
  str=ClInstanceToString(cli);
  if (strstr(str,"Instance") == NULL) {
    printf("Failed to get string from instance:%s\n",str);
    fail = 1;
  }
  str=ClInstanceGetNameSpace(cli);
  if (strcmp(str,"root") != 0) {
    printf("Failed to get namespace from instance:%s\n",str);
    fail = 1;
  }
  str=ClInstanceGetClassName(cli);
  if (strcmp(str,"myClass") != 0) {
    printf("Failed to get namespace from instance:%s\n",str);
    fail = 1;
  }

  ClClass * clc=ClClassNew("myClass","myPClass");
  if (clc == NULL) {
    printf("Failed to create ClClass.\n");
    fail = 1;
  }
  //unsigned char cch=ClClassAddGrandParent(clc, "myGClass");
  // not sure of the failure case here

  i=ClClassAddProperty(clc,"tprop" ,d,"testref");
  if (i != 1) {
    printf("Failed to add property to ClClass.\n");
    fail = 1;
  }
  ClClass *clc2=ClClassRebuildClass(clc,NULL);
  if (clc2 == NULL) {
    printf("Failed to rebuild ClClass.\n");
    fail = 1;
  }

  i=ClClassGetPropertyCount(clc);
  if (i != 1) {
    printf("Failed to get property count for ClClass.\n");
    fail = 1;
  }

  li=ClSizeClass(clc);
  if (li == 0) {
    printf("Failed to size ClClass.\n");
    fail = 1;
  }
  str=ClClassToString(clc);
  if (strstr(str,"class") == NULL) {
    printf("Failed to get string from ClClass:%s\n",str);
    fail = 1;
  }

  //Args tests
  ClArgs * cla=ClArgsNew();
  if (cla == NULL) {
    printf("Failed to create ClArgs object.\n");
    fail = 1;
  }
  li=ClSizeArgs(cla);
  if (li == 0) {
    printf("Failed to size ClArgs.\n");
    fail = 1;
  }
  ClArgs *cla2=ClArgsRebuild(cla,NULL);
  if (cla2 == NULL) {
    printf("Failed to rebuild ClArgs object.\n");
    fail = 1;
  }
  i=ClArgsAddArg(cla,"test",d);
  if (i == 0) {
    printf("Failed to add arg to ClArgs.\n");
    fail = 1;
  }
  i=ClArgsGetArgCount(cla);
  if (i == 0) {
    printf("Failed to get arg count.\n");
    fail = 1;
  }
  i=ClArgsGetArgAt(cla, 2, NULL, NULL);
  if (i != 1) {
    printf("Failed to catch bad arg count.\n");
    fail = 1;
  }

// Qualifiers
  ClQualifierDeclaration * clq=ClQualifierDeclarationNew("root","myQual");
  if (clq == NULL) {
    printf("Failed to create ClQualifier object.\n");
    fail = 1;
  }
  li=ClSizeQualifierDeclaration(clq);
  if (li == 0) {
    printf("Failed to size ClQualifier.\n");
    fail = 1;
  }
  ClQualifierDeclaration *clq2=ClQualifierRebuildQualifier(clq,NULL);
  if (clq2 == NULL) {
    printf("Failed to rebuild ClQualifier object.\n");
    fail = 1;
  }

  ClClassFreeClass(clc);
  ClInstanceFree(cli);
  ClObjectPathFree(clop);
  ClArgsFree(cla);

  // Return the final result
  return fail;
}
#endif

/* MODELINES */
/* DO NOT EDIT BELOW THIS COMMENT */
/* Modelines are added by 'make pretty' */
/* -*- Mode: C; c-basic-offset: 2; indent-tabs-mode: nil; -*- */
/* vi:set ts=2 sts=2 sw=2 expandtab: */
