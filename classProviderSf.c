
/*
 * classProvider.c
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
 * Author:       Adrian Schuur <schuur@de.ibm.com>
 *
 * Description:
 *
 * Class provider for sfcb .
 *
 */

#include "classProviderCommon.h"
#include <unistd.h>
#include <getopt.h>
#include <zlib.h>

#define LOCALCLASSNAME "ClassProvider"

static int      argc = 0;
static char   **argv = NULL;
static int      cSize = 10;      // can't be 0!
static int      rSize = 10;      // can't be 0!

typedef enum readCtl { stdRead, tempRead, cached } ReadCtl;

extern int      oiTrace;

typedef struct _Class_Register_FT Class_Register_FT;
struct _ClassRegister {
  void           *hdl;
  Class_Register_FT *ft;
  ClVersionRecord *vr;
  int             assocs,
                  topAssocs;
  char           *fn;
  gzFile          f;
};
typedef struct _ClassRegister ClassRegister;

typedef struct _ClassRecord {
  struct _ClassRecord *nextCCached,
                 *prevCCached;
  struct _ClassRecord *nextRCached,
                 *prevRCached;
  char           *parent;
  z_off_t         position;
  long            length;
  CMPIConstClass *cachedCCls;
  CMPIConstClass *cachedRCls;
  unsigned int    flags;
#define CREC_isAssociation 1
} ClassRecord;

typedef struct _ClassBase {
  UtilHashTable  *ht;
  UtilHashTable  *it;
  MRWLOCK         mrwLock;
  ClassRecord    *firstCCached,
                 *lastCCached;
  ClassRecord    *firstRCached,
                 *lastRCached;
  int             cachedCCount,
                  cachedRCount;
} ClassBase;

struct _Class_Register_FT {
  int             version;
  void            (*release) (ClassRegister * br);
  CMPIConstClass *(*getResolvedClass) (ClassRegister * br,
                                       const char *clsName,
                                       ClassRecord * crec, ReadCtl * ctl);
  CMPIConstClass *(*getClass) (ClassRegister * br, const char *clsName,
                               ReadCtl * ctl);
  int             (*putClass) (ClassRegister * br, const char *className,
                               ClassRecord * cls);
  void            (*removeClass) (ClassRegister * br,
                                  const char *className);
  void            (*releaseClass) (ClassRegister * br, void *id);

  UtilList       *(*getChildren) (ClassRegister * br,
                                  const char *className);
  void            (*addChild) (ClassRegister * cr, const char *p,
                               const char *child);

  Iterator        (*getFirstClassRecord) (ClassRegister * cr, char **cn,
                                          ClassRecord ** crec);
  Iterator        (*getNextClassRecord) (ClassRegister * cr,
                                         Iterator i, char **cn,
                                         ClassRecord ** crec);

  void            (*rLock) (ClassRegister * cr);
  void            (*wLock) (ClassRegister * cr);
  void            (*rUnLock) (ClassRegister * cr);
  void            (*wUnLock) (ClassRegister * cr);
};

extern Class_Register_FT *ClassRegisterFT;
extern int      ClClassAddQualifierSpecial(ClObjectHdr * hdr,
                                           ClSection * qlfs,
                                           const char *id, CMPIData d,
                                           ClObjectHdr * arrayHdr);
extern int      ClClassAddPropertyQualifierSpecial(ClObjectHdr * hdr,
                                                   ClProperty * p,
                                                   const char *id,
                                                   CMPIData d,
                                                   ClObjectHdr * arrayHdr);
extern void     sfcb_native_release_CMPIValue(CMPIType type,
                                              CMPIValue * val);
extern char   **buildArgList(char *str, char *pgm, int *argc);

static CMPIConstClass *getResolvedClass(ClassRegister * cr,
                                        const char *clsName,
                                        ClassRecord * crec, ReadCtl * ctl);
static CMPIConstClass *getClass(ClassRegister * cr, const char *clsName,
                                ReadCtl * ctl);
int             traverseChildren(ClassRegister * cReg, const char *parent,
                                 const char *child);

static int      nsBaseLen;

extern unsigned long exFlags;

static void
buildInheritanceTable(ClassRegister * cr)
{
  Iterator        i;
  char           *cn;
  ClassRecord    *crec;

  for (i = cr->ft->getFirstClassRecord(cr, &cn, &crec); i;
       i = cr->ft->getNextClassRecord(cr, i, &cn, &crec)) {
    if (crec->parent == NULL)
      continue;
    cr->ft->addChild(cr, crec->parent, cn);
  }
}

static void
release(ClassRegister * cr)
{
  ClassBase      *cb = (ClassBase *) cr->hdl;
  free(cr->fn);
  cb->ht->ft->release(cb->ht);
  free(cr);
}

void
rLock(ClassRegister * cr)
{
  ClassBase      *cb = (ClassBase *) cr->hdl;
  MReadLock(&cb->mrwLock);
}

void
wLock(ClassRegister * cr)
{
  ClassBase      *cb = (ClassBase *) cr->hdl;
  MWriteLock(&cb->mrwLock);
}

void
rUnLock(ClassRegister * cr)
{
  ClassBase      *cb = (ClassBase *) cr->hdl;
  MReadUnlock(&cb->mrwLock);
}

void
wUnLock(ClassRegister * cr)
{
  ClassBase      *cb = (ClassBase *) cr->hdl;
  MWriteUnlock(&cb->mrwLock);
}

static Iterator
getFirstClassRecord(ClassRegister * cr, char **cn, ClassRecord ** crec)
{
  ClassBase      *cb = (ClassBase *) cr->hdl;
  return cb->ht->ft->getFirst(cb->ht, (void **) cn, (void **) crec);
}

static Iterator
getNextClassRecord(ClassRegister * cr, Iterator i, char **cn,
                   ClassRecord ** crec)
{
  ClassBase      *cb = (ClassBase *) cr->hdl;
  return cb->ht->ft->getNext(cb->ht, i, (void **) cn, (void **) crec);
}

static UtilList *
getChildren(ClassRegister * cr, const char *className)
{
  ClassBase      *cb = (ClassBase *) (cr + 1);
  return cb->it->ft->get(cb->it, className);
}

static void
addChild(ClassRegister * cr, const char *p, const char *child)
{
  UtilList       *ul =
      ((ClassBase *) (cr + 1))->it->ft->get(((ClassBase *) (cr + 1))->it,
                                            p);
  if (ul == NULL) {
    ul = UtilFactory->newList(memAddUtilList, memUnlinkEncObj);
    ((ClassBase *) (cr + 1))->it->ft->put(((ClassBase *) (cr + 1))->it, p,
                                          ul);
  }
  ul->ft->prepend(ul, child);
}

void
releaseClass(CMPIConstClass * cls, char *from, int id)
{
  printf("### RELEASE %s %p %s %d\n", cls->ft->getCharClassName(cls), cls,
         from, id);
  CMRelease(cls);
}

// static void
// clearCCache(ClassRegister * cr)
// {
//   ClassBase      *cb = (ClassBase *) (cr + 1);
//   ClassRecord    *crec;

//   while (cb->cachedCCount > 0) {
//     crec = cb->lastCCached;
//     DEQ_FROM_LIST(crec, cb->firstCCached, cb->lastCCached, nextCCached,
//                   prevCCached);
//     CMRelease(crec->cachedCCls);
//     crec->cachedCCls = NULL;
//     cb->cachedCCount--;
//   }
// }
// static void
// clearRCache(ClassRegister * cr)
// {
//   ClassBase      *cb = (ClassBase *) (cr + 1);
//   ClassRecord    *crec;

//   while (cb->cachedRCount > 0) {
//     crec = cb->lastRCached;
//     DEQ_FROM_LIST(crec, cb->firstRCached, cb->lastRCached, nextRCached,
//                   prevRCached);
//     CMRelease(crec->cachedRCls);
//     crec->cachedRCls = NULL;
//     cb->cachedRCount--;
//   }
// }

static void
pruneCCache(ClassRegister * cr)
{
  ClassBase      *cb = (ClassBase *) (cr + 1);
  ClassRecord    *crec;

  while (cb->cachedCCount > cSize) {
    crec = cb->lastCCached;
    DEQ_FROM_LIST(crec, cb->firstCCached, cb->lastCCached, nextCCached,
                  prevCCached);
    // char* claz = CMGetCharPtr(crec->cachedCCls->ft->getClassName(crec->cachedCCls, NULL));
    // fprintf(stderr, "--- pruning Ccache: %s\n", claz); 
    CMRelease(crec->cachedCCls);
    crec->cachedCCls = NULL;
    cb->cachedCCount--;
  }
}

static void
pruneRCache(ClassRegister * cr)
{
  ClassBase      *cb = (ClassBase *) (cr + 1);
  ClassRecord    *crec;

  while (cb->cachedRCount > rSize) {
    crec = cb->lastRCached;
    DEQ_FROM_LIST(crec, cb->firstRCached, cb->lastRCached, nextRCached,
                  prevRCached);
    // char* claz = CMGetCharPtr(crec->cachedRCls->ft->getClassName(crec->cachedRCls, NULL));
    // fprintf(stderr, "--- pruning Rcache: %s\n", claz); 
    // if (strcasecmp("CIM_LogicalElement", claz)) {
    //   fprintf(stderr, "   qual count=%d\n", Clcrec->cachedRCls
    // }
    CMRelease(crec->cachedRCls);
    crec->cachedRCls = NULL;
    cb->cachedRCount--;
  }
}

static ClassRegister *
newClassRegister(char *fname)
{
  ClassRegister  *cr = calloc(1, sizeof(*cr) + sizeof(ClassBase));
  ClassBase      *cb = (ClassBase *) (cr + 1);
  char            fin[1024];
  long            s,
                  total = 0;
  ClObjectHdr     hdr;
  ClVersionRecord *vrp = (ClVersionRecord *) & hdr;
  int             vRec = 0,
      first = 1;

  z_off_t         pos = 0;
  ClassRecord    *crec;

  cr->hdl = cb;
  cr->ft = ClassRegisterFT;
  cr->vr = NULL;
  cr->assocs = cr->topAssocs = 0;

  strcpy(fin, fname);
  strcat(fin, "/classSchemas");
  cr->f = gzopen(fin, "r");
  if (cr->f == NULL) {
    strcat(fin, ".gz");
    cr->f = gzopen(fin, "r");
  }

  cb->ht = UtilFactory->newHashTable(61,
                                     UtilHashTable_charKey |
                                     UtilHashTable_ignoreKeyCase);
  cb->it =
      UtilFactory->newHashTable(61,
                                UtilHashTable_charKey |
                                UtilHashTable_ignoreKeyCase);
  MRWInit(&cb->mrwLock);

  if (cr->f == NULL)
    return cr;

  cr->fn = strdup(fin);
  cr->vr = NULL;
  pos = gztell(cr->f);

  while ((s = gzread(cr->f, &hdr, sizeof(hdr))) == sizeof(hdr)) {
    CMPIConstClass *cc = NULL;
    char           *buf = NULL;
    const char     *cn,
                   *pn;

    if (first) {
      if (vrp->size == sizeof(ClVersionRecord) && vrp->type == HDR_Version)
        vRec = 1;
      else if (vrp->size == sizeof(ClVersionRecord) << 24
               && vrp->type == HDR_Version) {
        mlogf(M_ERROR, M_SHOW,
              "--- %s is in wrong endian format - directory skipped\n",
              fin);
        return NULL;
      }
    }

    if (vRec == 0 && hdr.type != HDR_Class
        && hdr.type != HDR_IncompleteClass) {
      mlogf(M_ERROR, M_SHOW,
            "--- %s contains non-class record(s) - directory skipped %d\n",
            fin, hdr.type);
      return NULL;
    }

    buf = malloc(hdr.size);
    if (buf == NULL) {
      mlogf(M_ERROR, M_SHOW,
            "--- %s contains record(s) that are too long - directory skipped\n",
            fin);
      return NULL;
    }

    s = hdr.size;
    *((ClObjectHdr *) buf) = hdr;

    int gzr = gzread(cr->f, buf + sizeof(hdr), hdr.size - sizeof(hdr));
    if (gzr && (unsigned int)gzr == hdr.size - sizeof(hdr)) {
      if (vRec) {
        cr->vr = (ClVersionRecord *) buf;
        if (strcmp(cr->vr->id, "sfcb-rep")) {
          mlogf(M_ERROR, M_SHOW,
                "--- %s contains invalid version record - directory skipped\n",
                fin);
          return NULL;
        }
        pos = gztell(cr->f);
        vRec = 0;
      }

      if (first) {
        int v = -1;
        int r = -1;
        first = 0;
	if (ClVerifyObjImplLevel(cr->vr)) 
	  continue;

        if (cr->vr)
          v = cr->vr->objImplLevel;
        mlogf(M_ERROR, M_SHOW,
              "--- %s contains unsupported object implementation format (level %d%s) - directory skipped\n",
              fin, v, (r) ? ", reduced" : ", not reduced" );
        return NULL;
      }

      cc = NEW(CMPIConstClass);
      cc->hdl = buf;
      cc->ft = CMPIConstClassFT;
      cc->ft->relocate(cc);
      cn = (char *) cc->ft->getCharClassName(cc);

      if (strncmp(cn, "DMY_", 4) != 0) {
        total += sizeof(ClassRecord);
        crec = calloc(1, sizeof(*crec));

        if ((pn = cc->ft->getCharSuperClassName(cc))) {
          crec->parent = strdup(pn);
        }
        crec->position = pos;
        crec->length = s;
        cr->ft->putClass(cr, strdup(cn), crec);

        if (cc->ft->isAssociation(cc)) {
          crec->flags |= CREC_isAssociation;
          cr->assocs++;
          if (pn == NULL)
            cr->topAssocs++;
        }
      }
      first = 0;
    } else {
      mlogf(M_ERROR, M_SHOW,
            "--- %s contains invalid record(s) - directory skipped\n",
            fin);
      return NULL;
    }
    pos = gztell(cr->f);
    free(buf);
    free(cc);
  }

  if (cr->vr) {
    mlogf(M_INFO, M_SHOW,
          "--- Caching ClassProviderSf for %s (%d.%d-%d) using %ld bytes\n",
          fin, cr->vr->version, cr->vr->level, cr->vr->objImplLevel,
          total);
  } else
    mlogf(M_INFO, M_SHOW,
          "--- Caching ClassProviderSf for %s (no-version) using %ld bytes\n",
          fin, total);

  buildInheritanceTable(cr);

  return cr;
}

static UtilHashTable *
gatherNameSpaces(char *dn, UtilHashTable * ns, int first)
{
  DIR            *dir,
                 *dir_test;
  struct dirent  *de;
  char           *n = NULL;
  int             l;
  ClassRegister  *cr;

  if (ns == NULL) {
    ns = UtilFactory->newHashTable(61,
                                   UtilHashTable_charKey |
                                   UtilHashTable_ignoreKeyCase);
    nsBaseLen = strlen(dn) + 1;
  }

  dir = opendir(dn);
  if (dir)
    while ((de = readdir(dir)) != NULL) {
      if (strcmp(de->d_name, ".") == 0)
        continue;
      if (strcmp(de->d_name, "..") == 0)
        continue;
      if (!(exFlags & 2)) {
        /* enforce enableInterOp = false [sfcb#90] */
        if (strncmp(de->d_name, "interop", 7) == 0) {
          continue;
        }
      }
      l = strlen(dn) + strlen(de->d_name) + 4;
      n = malloc(l + 8);
      strcpy(n, dn);
      strcat(n, "/");
      strcat(n, de->d_name);
      dir_test = opendir(n);
      if (dir_test == NULL) {
        free(n);
        continue;
      }
      closedir(dir_test);
      cr = newClassRegister(n);
      if (cr) {
	/* couldn't open classSchemas, don't put it in the ht */
        if (cr->f)
          ns->ft->put(ns, strdup(n + nsBaseLen), cr);
        gatherNameSpaces(n, ns, 0);
      }
      free(n);
  } else if (first) {
    mlogf(M_ERROR, M_SHOW, "--- Repository %s not found\n", dn);
  }
  closedir(dir);
  return ns;
}

static UtilHashTable *
buildClassRegisters()
{
  char           *dir;
  char           *dn;

  setupControl(configfile);

  if (getControlChars("registrationDir", &dir)) {
    dir = "/var/lib/sfcb/registration";
  }

  dn = alloca(strlen(dir) + 32);
  strcpy(dn, dir);
  if (dir[strlen(dir) - 1] != '/')
    strcat(dn, "/");
  strcat(dn, "repository");
  return gatherNameSpaces(dn, NULL, 1);
}

static void
nsHt_init()
{
  nsHt = buildClassRegisters();
}

static ClassRegister *
getNsReg(const CMPIObjectPath * ref, int *rc)
{
  char           *ns;
  CMPIString     *nsi = CMGetNameSpace(ref, NULL);
  ClassRegister  *cReg;
  *rc = 0;

  pthread_once(&nsHt_once, nsHt_init);

  if (nsHt == NULL) {
    mlogf(M_ERROR, M_SHOW,
          "--- ClassProvider: namespace hash table not initialized\n");
    *rc = 1;
    return NULL;
  }

  if (nsi && nsi->hdl) {
    ns = (char *) nsi->hdl;
    cReg = nsHt->ft->get(nsHt, ns);
    return cReg;
  }

  *rc = 1;
  return NULL;
}

static int
putClass(ClassRegister * cr, const char *cn, ClassRecord * crec)
{
  ClassBase      *cb = (ClassBase *) cr->hdl;
  return cb->ht->ft->put(cb->ht, cn, crec);
}

/*
 cl - class we're copying into
 cc - class we're copying from (parent)
 */
static int
cpyClass(ClClass * cl, CMPIConstClass * cc)
{
  ClClass        *ccl = (ClClass *) cc->hdl;
  CMPIData        d;
  CMPIParameter p;
  CMPIType t;
  char           *name;
  char           *refName = NULL;
  int             i,
                  m,
                  iq,
                  mq,
                  propId;
  int parmId, methId, mp, ip;
  unsigned long   quals;
  ClProperty     *prop;
  ClMethod *meth, *pmeth;
  ClParameter *parm, *pparm;

  /* copy class qualifiers */
  cl->quals |= ccl->quals;
  for (i = 0, m = ClClassGetQualifierCount(ccl); i < m; i++) {
    ClClassGetQualifierAt(ccl, i, &d, &name);
    ClClassAddQualifierSpecial(&cl->hdr, &cl->qualifiers, name, d,
                               &ccl->hdr);
    if (!(d.type & CMPI_ARRAY)) {
      sfcb_native_release_CMPIValue(d.type, &d.value);
    }
  }

  /* copy properties */
  for (i = 0, m = ClClassGetPropertyCount(ccl); i < m; i++) {
    char           *pname;
    ClClassGetPropertyAt(ccl, i, &d, &pname, &quals, &refName);

    propId = ClClassAddProperty(cl, pname, d, refName);
    prop =
        ((ClProperty *) ClObjectGetClSection(&cl->hdr, &cl->properties)) +
        propId - 1;

    /* copy prop qualifiers, such as "key" */
    prop->quals |= quals;


    /* copy property qualifiers */
    for (iq = 0, mq = ClClassGetPropQualifierCount(ccl, i); iq < mq; iq++) {
      char           *qname;
      ClClassGetPropQualifierAt(ccl, i, iq, &d, &qname);
      ClClassAddPropertyQualifierSpecial(&cl->hdr, prop, qname, d,
                                         &ccl->hdr);
      if (!(d.type & CMPI_ARRAY))
        sfcb_native_release_CMPIValue(d.type, &d.value);
    }
  }

  // const char* ccl_name = ClObjectGetClString(&ccl->hdr, &ccl->name);

  /* copy methods */
 for (i=0,m=ClClassGetMethodCount(ccl); i<m; i++) {
   //   fprintf(stderr, " cpyClass %s: %d methods\n", ccl_name, ClClassGetMethodCount(ccl));
   ClClassGetMethodAt(ccl,i,&t,&name,&quals);
   //   fprintf(stderr, "  meth %s, ", name);
   methId=ClClassAddMethod(cl, name, t);

   meth=((ClMethod*)ClObjectGetClSection(&cl->hdr,&cl->methods))+methId-1;
   pmeth=((ClMethod*)ClObjectGetClSection(&ccl->hdr,&ccl->methods))+i;

   mq = ClClassGetMethQualifierCount(ccl,i);
   //   fprintf(stderr, "qual count = %d\n", mq);
   for (iq=0; iq<mq; iq++) { 
     ClClassGetMethQualifierAt(ccl, pmeth, iq, &d, &name);
     //     fprintf(stderr, "     adding meth qual %s\n", name);
     ClClassAddMethodQualifier(&cl->hdr, meth, name, d); 
   }
  
   mp=ClClassGetMethParameterCount(ccl,i);
   //     fprintf(stderr, "   meth param count (of parent) = %d\n", mp);
   for (ip=0; ip<mp; ip++) { 
      ClClassGetMethParameterAt(ccl, pmeth, ip, &p, &name);
      // fprintf(stderr, "cpyClass: param %s:\n", name);
      CMPIParameter cp; 
      cp.type = p.type;
      cp.arraySize = p.arraySize;
      cp.refName = p.refName;

      //      fprintf(stderr, "     meth param: %s\n", name);
      parmId=ClClassAddMethParameter(&cl->hdr, meth, name, cp);

      parm = ((ClParameter*)ClObjectGetClSection(&cl->hdr,&meth->parameters))+parmId-1;
      pparm = ((ClParameter*)ClObjectGetClSection(&ccl->hdr,&pmeth->parameters))+ip;
      mq=ClClassGetMethParamQualifierCount(parm);
      //      fprintf(stderr, "       meth param qual count = %d\n", mq);
      for (iq=0; iq<mq; iq++) { 
      	ClClassGetMethParamQualifierAt(ccl, pparm, iq, &d, &name);
      	ClClassAddMethParamQualifier(&cl->hdr, parm, name, d);
      }
   }
 }

 // cl->hdr.flags &= ~HDR_Rebuild; /* force it - CJB */
 return 0;
}

/*
  p is parent class
  cl is new class (empty)
  cc is the class
 */
static CMPIStatus
mergeParents(ClassRegister * cr, ClClass * cl, char *p,
             CMPIConstClass * cc, ReadCtl * rctl)
{
  CMPIStatus      st = { CMPI_RC_OK, NULL };
  CMPIConstClass *pcc = NULL;
  char           *np = NULL;
  ReadCtl         ctl;

  if (cc) {
    cpyClass(cl, cc);
  }
  if (p) {
    ctl = *rctl;
    pcc = getClass(cr, p, &ctl);
    if (pcc == NULL) {
      st.rc = CMPI_RC_ERR_INVALID_SUPERCLASS;
      return st;
    }
    np = (char *) pcc->ft->getCharSuperClassName(pcc);
    st = mergeParents(cr, cl, np, pcc, rctl);
    if (ctl != cached)
      CMRelease(pcc);

  }
  return st;
}

static CMPIConstClass *
getResolvedClass(ClassRegister * cr, const char *clsName,
                 ClassRecord * crec, ReadCtl * rctl)
{
  _SFCB_ENTER(TRACE_PROVIDERS, "getResolvedClass");
  _SFCB_TRACE(1, ("--- classname %s cReg %p", clsName, cr));

  /* if standard repo, skip straight to normal gc */
  if (cr->vr->options != ClTypeClassReducedRep) {
    return getClass(cr, clsName, rctl);
  }

  CMPIConstClass *cc = NULL,
      *cls = NULL;
  ReadCtl         ctl = *rctl;
  ReadCtl         cls_ctl; /* the ctl for cls from getClass */
  ClassBase      *cb = (ClassBase *) cr->hdl;

  if (crec == NULL) {
    crec = cb->ht->ft->get(cb->ht, clsName);
    if (crec == NULL)
      _SFCB_RETURN(NULL);
  }

  if (crec->cachedRCls == NULL) {
    cls = getClass(cr, clsName, &ctl);
    cls_ctl = ctl;
    ClClass        *ccl = (ClClass *) cls->hdl;
    if (ccl->hdr.type == HDR_Class) {
      return cls;
    }

    char           *pn = (char *) cls->ft->getCharSuperClassName(cls);
    if (pn == NULL) {
      *rctl = ctl;
      return cls;
    }

    ClClass        *mc = ClClassNew(clsName, pn);
    cc = NEW(CMPIConstClass);
    cc->ft = CMPIConstClassFT;
    cc->hdl = mc;
    // printf("#-# merging %s %s\n",clsName,pn);
    _SFCB_TRACE(1, ("#-# merging %s %s\n",clsName,pn));

    char orig_abst = mc->quals & ClClass_Q_Abstract;

    mergeParents(cr, mc, pn, cls, rctl);
    /* We want all our parents' qualifiers except for Abstract. 
       Clear and set using original Abstract bit */
    mc->quals &= ~ClClass_Q_Abstract;
    mc->quals |= orig_abst;

    if (*rctl == tempRead) {
      if (cls && (cls_ctl != cached))
       	CMRelease(cls);
      _SFCB_RETURN(cc);
    }

    crec->cachedRCls = cc;
    cb->cachedRCount++;
    if (cb->cachedRCount >= rSize) {
      pruneRCache(cr);
    }
    ENQ_TOP_LIST(crec, cb->firstRCached, cb->lastRCached, nextRCached,
                 prevRCached);
  } else {
    // printf("-#- class %s in resolved cache
    // %p\n",clsName,crec->cachedRCls);
    _SFCB_TRACE(1, ("-#- class %s in resolved cache %p\n",clsName,crec->cachedRCls));
    if (crec != cb->firstRCached) {
      DEQ_FROM_LIST(crec, cb->firstRCached, cb->lastRCached, nextRCached,
                    prevRCached);
      ENQ_TOP_LIST(crec, cb->firstRCached, cb->lastRCached, nextRCached,
                   prevRCached);
    }
  }
  *rctl = cached;
  _SFCB_RETURN(crec->cachedRCls);
}

static CMPIConstClass *
getClass(ClassRegister * cr, const char *clsName, ReadCtl *ctl)
{
  ClassRecord    *crec;
  CMPIConstClass *cc;
  char           *buf;

  _SFCB_ENTER(TRACE_PROVIDERS, "getClass");
  _SFCB_TRACE(1, ("--- classname %s cReg %p", clsName, cr));
  ClassBase      *cb = (ClassBase *) cr->hdl;

  crec = cb->ht->ft->get(cb->ht, clsName);
  if (crec == NULL) {
    _SFCB_RETURN(NULL);
  }

  /* class is not cached */
  if (crec->cachedCCls == NULL) {
    gzseek(cr->f, crec->position, SEEK_SET);
    buf = malloc(crec->length);
    gzread(cr->f, buf, crec->length);

    cc = NEW(CMPIConstClass);
    cc->hdl = buf;
    cc->ft = CMPIConstClassFT;
    cc->ft->relocate(cc);

    //    char* claz = CMGetCharPtr(cc->ft->getClassName(cc, NULL));

    if (*ctl == tempRead) {
      _SFCB_RETURN(cc);
    }
    //    printf("-#- class %s Added %p",clsName,cc);
    //    CMPIArray* ar = cc->ft->getKeyList(cc);
    //    printf("; key list size=%d\n", (int)ar->ft->getSize(ar, NULL));

    crec->cachedCCls = cc;
    cb->cachedCCount++;
    if (cb->cachedCCount >= cSize)
      pruneCCache(cr);
    //    fprintf(stderr, "ENQing %p\n", crec);
    ENQ_TOP_LIST(crec, cb->firstCCached, cb->lastCCached, nextCCached,
                 prevCCached);
    *ctl = cached;
  } else {
    //    printf("-#- class %s in cache %p\n",clsName,crec->cachedCCls);
    if (crec != cb->firstCCached) {
      DEQ_FROM_LIST(crec, cb->firstCCached, cb->lastCCached, nextCCached,
                    prevCCached);
      //    fprintf(stderr, "ENQing %p\n", crec);
      ENQ_TOP_LIST(crec, cb->firstCCached, cb->lastCCached, nextCCached,
                   prevCCached);
    }
  }
  *ctl = cached;
  _SFCB_RETURN(crec->cachedCCls);
}

static Class_Register_FT ift = {
  1,
  release,
  getResolvedClass,
  getClass,
  putClass,
  NULL,
  NULL,
  getChildren,
  addChild,
  getFirstClassRecord,
  getNextClassRecord,
  rLock,
  wLock,
  rUnLock,
  wUnLock
};

Class_Register_FT *ClassRegisterFT = &ift;

static CMPIStatus
initialize(CMPIClassMI * mi, CMPIContext *ctx)
{

  CMPIStatus      st;
  char           *p,
                  c;

  /*
   * to set params, in providerRegister add line: parameters: -c10 -r15 
   */
  static struct option const long_options[] = {
    {"base-class-cache", required_argument, 0, 'c'},
    {"resolved-class-cache", required_argument, 0, 'r'},
    {0, 0, 0, 0}
  };
  char            msg[] =
      "--- Invalid classProviderSf parameter -%c %s ignored \n";
  CMPIData        parms =
      ctx->ft->getEntry(ctx, "sfcbProviderParameters", &st);

  if (st.rc == 0) {
    argv =
        buildArgList((char *) parms.value.string->hdl, "classProviderSf",
                     &argc);
    mlogf(M_INFO, M_SHOW, "--- %s parameters: %s\n", argv[0],
          (char *) parms.value.string->hdl);
    while ((c = getopt_long(argc, argv, "c:r:", long_options, 0)) != -1) {
      switch (c) {
      case 0:
        break;
      case 'c':
        if (isdigit(*optarg))
          cSize = strtol(optarg, &p, 0);
        else
          mlogf(M_INFO, M_SHOW, msg, c, optarg);
        break;
      case 'r':
        if (isdigit(*optarg))
          rSize = strtol(optarg, &p, 0);
        else
          mlogf(M_INFO, M_SHOW, msg, c, optarg);
        break;
      default:
        mlogf(M_INFO, M_SHOW, msg, c, optarg);
      }
    }
  } else {
  }

  // if (nsHt==NULL) nsHt=buildClassRegisters(); why is this here? CJB
  CMReturn(CMPI_RC_OK);
}

/*
 * ------------------------------------------------------------------ *
 * Class MI Cleanup
 * ------------------------------------------------------------------ 
 */

static CMPIStatus
ClassProviderCleanup(CMPIClassMI * mi, const CMPIContext *ctx)
{

  // int rc = 0;
  // CMPIObjectPath* ref = CMNewObjectPath(_broker, "root/interop", "cim_managedelement", NULL);
  // ClassRegister* cReg = getNsReg(ref, &rc);
  // clearCCache(cReg);
  // clearRCache(cReg);

  // HashTableIterator *hit,
  //                *hitHt,
  //                *hitIt;
  // char           *key;
  // ClassRegister  *cReg;
  // ClassRecord    *crec;
  // UtilList       *ul;
  // fprintf(stderr, "1nsHT has %d/%d\n", nsHt->ft->size(nsHt),  nsHt->ft->buckets(nsHt));

  // for (hit = nsHt->ft->getFirst(nsHt, (void **) &key, (void **) &cReg);
  //      key && hit && cReg;
  //      hit =
  //      nsHt->ft->getNext(nsHt, hit, (void **) &key, (void **) &cReg)) {
  //   gzclose(cReg->f);
  //   free(cReg->vr);
  //   free(cReg->fn);

  //   ClassBase      *cb = (ClassBase *) (cReg + 1);
  //   for (hitIt =
  //        cb->it->ft->getFirst(cb->it, (void **) &key, (void **) &ul);
  //        key && hitIt && ul;
  //        hitIt =
  //        cb->it->ft->getNext(cb->it, hitIt, (void **) &key,
  //                            (void **) &ul)) 
  //     {
  //     if (ul)
  //       CMRelease(ul);
  //   }
  //   CMRelease(cb->it);

  //   for (hitHt =
  //        cb->ht->ft->getFirst(cb->ht, (void **) &key, (void **) &crec);
  //        key && hitHt && crec;
  //        hitHt =
  //        cb->ht->ft->getNext(cb->ht, hitHt, (void **) &key,
  //                            (void **) &crec)) {
  //     free(key);
  //     if (crec->parent)
  //       free(crec->parent);
  //     free(crec);
  //   }
  //   CMRelease(cb->ht);
  //   free(cReg);
  // }
  // fprintf(stderr, "nsHT has %d/%d\n", nsHt->ft->size(nsHt),  nsHt->ft->buckets(nsHt));
  // CMRelease(nsHt);
  CMReturn(CMPI_RC_OK);
}

/*
 * ------------------------------------------------------------------ *
 * Class MI Functions
 * ------------------------------------------------------------------ 
 */

static void
loopOnChildNames(ClassRegister * cReg, char *cn, const CMPIResult *rslt)
{
  CMPIObjectPath *op;
  UtilList       *ul = getChildren(cReg, cn);
  char           *child;
  if (ul)
    for (child = (char *) ul->ft->getFirst(ul); child;
         child = (char *) ul->ft->getNext(ul)) {
      op = CMNewObjectPath(_broker, NULL, child, NULL);
      CMReturnObjectPath(rslt, op);
      loopOnChildNames(cReg, child, rslt);
    }
}

static CMPIStatus
ClassProviderEnumClassNames(CMPIClassMI * mi,
                            const CMPIContext *ctx,
                            const CMPIResult *rslt, const CMPIObjectPath * ref)
{
  CMPIStatus      st = { CMPI_RC_OK, NULL };
  char           *cn = NULL;
  CMPIFlags       flgs = 0;
  CMPIString     *cni;
  Iterator        it;
  char           *key;
  int             rc;
  ClassRecord    *crec;
  CMPIObjectPath *op;
  ClassRegister  *cReg;
  char           *ns;

  _SFCB_ENTER(TRACE_PROVIDERS, "ClassProviderEnumClassNames");

  cReg = getNsReg(ref, &rc);
  if (cReg == NULL) {
    CMPIStatus      st = { CMPI_RC_ERR_INVALID_NAMESPACE, NULL };
    _SFCB_RETURN(st);
  }

  ns = (char *) CMGetNameSpace(ref, NULL)->hdl;
  flgs = ctx->ft->getEntry(ctx, CMPIInvocationFlags, NULL).value.uint32;
  cni = ref->ft->getClassName(ref, NULL);
  if (cni) {
    cn = (char *) cni->hdl;
    if (cn && *cn == 0)
      cn = NULL;
  }

  cReg->ft->wLock(cReg);

  if (cn && strcasecmp(cn, "$ClassProvider$") == 0)
    cn = NULL;

  if (cn == NULL) {
    for (it = cReg->ft->getFirstClassRecord(cReg, &key, &crec);
         key && it && crec;
         it = cReg->ft->getNextClassRecord(cReg, it, &key, &crec)) {
      if ((flgs & CMPI_FLAG_DeepInheritance) || crec->parent == NULL) {
        if (((flgs & FL_assocsOnly) == 0)
            || crec->flags & CREC_isAssociation) {
          op = CMNewObjectPath(_broker, ns, key, NULL);
          CMReturnObjectPath(rslt, op);
        }
      }
    }
  }

  else {
    ReadCtl         ctl = tempRead;
    CMPIConstClass *cls = getClass(cReg, cn, &ctl);
    if (cls == NULL) {
      st.rc = CMPI_RC_ERR_INVALID_CLASS;
    } else if ((flgs & CMPI_FLAG_DeepInheritance) == 0) {
      UtilList       *ul = getChildren(cReg, cn);
      char           *child;
      if (ul)
        for (child = (char *) ul->ft->getFirst(ul); child;
             child = (char *) ul->ft->getNext(ul)) {
          op = CMNewObjectPath(_broker, ns, child, NULL);
          CMReturnObjectPath(rslt, op);
        }
    } else if (flgs & CMPI_FLAG_DeepInheritance) {
      if (((flgs & FL_assocsOnly) == 0)
          || crec->flags & CREC_isAssociation)
        loopOnChildNames(cReg, cn, rslt);
    }
  }

  cReg->ft->wUnLock(cReg);

  _SFCB_RETURN(st);
}

static void
loopOnChildren(ClassRegister * cReg, char *cn, const CMPIResult *rslt)
{
  UtilList       *ul = getChildren(cReg, cn);
  char           *child;
  ReadCtl         ctl;

  if (ul)
    for (child = (char *) ul->ft->getFirst(ul); child;
         child = (char *) ul->ft->getNext(ul)) {
      ctl = tempRead;
      CMPIConstClass *cl = getResolvedClass(cReg, child, NULL, &ctl);
      CMReturnInstance(rslt, (CMPIInstance *) cl);
      if (ctl != cached) {
        CMRelease(cl);
      }
      loopOnChildren(cReg, child, rslt);
    }
}

static CMPIStatus
ClassProviderEnumClasses(CMPIClassMI * mi,
                         const CMPIContext *ctx,
                         const CMPIResult *rslt, const CMPIObjectPath * ref)
{
  CMPIStatus      st = { CMPI_RC_OK, NULL };
  char           *cn = NULL;
  CMPIFlags       flgs = 0;
  CMPIString     *cni;
  Iterator        it;
  char           *key;
  int             rc;
  CMPIConstClass *cls;
  ClassRegister  *cReg;
  ReadCtl         rctl;
  ClassRecord    *crec;

  _SFCB_ENTER(TRACE_PROVIDERS, "ClassProviderEnumClasss");

  cReg = getNsReg(ref, &rc);
  if (cReg == NULL) {
    CMPIStatus      st = { CMPI_RC_ERR_INVALID_NAMESPACE, NULL };
    _SFCB_RETURN(st);
  }

  cReg->ft->wLock(cReg);

  flgs = ctx->ft->getEntry(ctx, CMPIInvocationFlags, NULL).value.uint32;
  cni = ref->ft->getClassName(ref, NULL);
  if (cni) {
    cn = (char *) cni->hdl;
    if (cn && *cn == 0)
      cn = NULL;
  }

  if (cn == NULL) {
    for (it = cReg->ft->getFirstClassRecord(cReg, &key, &crec);
         key && it && crec;
         it = cReg->ft->getNextClassRecord(cReg, it, &key, &crec)) {
      char           *cn = key;
      char           *pcn = crec->parent;
      if ((flgs & CMPI_FLAG_DeepInheritance) || pcn == NULL) {
        rctl = tempRead;
        CMPIConstClass *rcls = getResolvedClass(cReg, cn, crec, &rctl);
        CMReturnInstance(rslt, (CMPIInstance *) rcls);
        if (rctl != cached) {
          CMRelease(rcls);
	}
      }
    }
  }

  else {
    rctl = tempRead;
    cls = getResolvedClass(cReg, cn, NULL, &rctl);
    if (cls == NULL) {
      st.rc = CMPI_RC_ERR_INVALID_CLASS;
    } else if ((flgs & CMPI_FLAG_DeepInheritance) == 0) {
      if (rctl != cached)
	CMRelease(cls);
      UtilList       *ul = getChildren(cReg, cn);
      char           *child;
      if (ul) {
        for (child = (char *) ul->ft->getFirst(ul); child;
             child = (char *) ul->ft->getNext(ul)) {
          rctl = tempRead;
          cls = getResolvedClass(cReg, child, NULL, &rctl);
          CMReturnInstance(rslt, (CMPIInstance *) cls);
          if (rctl != cached) {
            CMRelease(cls);
	  }
        }
      }
    } else if (cn && (flgs & CMPI_FLAG_DeepInheritance)) {
      if (rctl != cached) {
	CMRelease(cls);
      }
      loopOnChildren(cReg, cn, rslt);
    }
  }

  cReg->ft->wUnLock(cReg);

  _SFCB_RETURN(st);
}

static CMPIStatus
ClassProviderGetClass(CMPIClassMI * mi,
                      const CMPIContext *ctx,
                      const CMPIResult *rslt,
                      const CMPIObjectPath * ref, const char **properties)
{
  CMPIStatus      st = { CMPI_RC_OK, NULL };
  CMPIString     *cn = CMGetClassName(ref, NULL);
  CMPIConstClass *cl,
                 *clLocal;
  ClassRegister  *cReg;
  int             rc;
  ReadCtl         ctl;

  _SFCB_ENTER(TRACE_PROVIDERS, "ClassProviderGetClass");
  _SFCB_TRACE(1, ("--- ClassName=\"%s\"", (char *) cn->hdl));

  cReg = getNsReg(ref, &rc);
  if (cReg == NULL) {
    CMPIStatus      st = { CMPI_RC_ERR_INVALID_NAMESPACE, NULL };
    _SFCB_RETURN(st);
  }

  cReg->ft->wLock(cReg);

  ctl = stdRead;
  clLocal = getResolvedClass(cReg, (char *) cn->hdl, NULL, &ctl);
  if (clLocal) {
    /*
     * Make a cloned copy of the cached results to prevent thread
     * interference. 
     */
    _SFCB_TRACE(1, ("--- Class found"));
    cl = clLocal->ft->clone(clLocal, NULL);
    memLinkInstance((CMPIInstance *) cl);
    if(properties) {
      filterClass(cl, properties);
    }
    CMReturnInstance(rslt, (CMPIInstance *) cl);
    if (ctl != cached)
      CMRelease(cl);
  } else {
    _SFCB_TRACE(1, ("--- Class not found"));
    st.rc = CMPI_RC_ERR_NOT_FOUND;
  }


  cReg->ft->wUnLock(cReg);

  _SFCB_RETURN(st);
}

static CMPIStatus
ClassProviderCreateClass(CMPIClassMI * mi,
                         const CMPIContext *ctx,
                         const CMPIResult *rslt,
                         const CMPIObjectPath * ref, const CMPIConstClass * cc)
{
  return notSupSt;
}

static CMPIStatus
ClassProviderSetClass(CMPIClassMI * mi,
                      const CMPIContext *ctx,
                      const CMPIResult *rslt,
                      const CMPIObjectPath * cop, const CMPIConstClass * ci)
{
  return notSupSt;
}

static CMPIStatus
ClassProviderDeleteClass(CMPIClassMI * mi,
                         const CMPIContext *ctx,
                         const CMPIResult *rslt, const CMPIObjectPath * cop)
{
  return notSupSt;
}

/*
 * ---------------------------------------------------------------------------
 */
/*
 * Method Provider Interface 
 */
/*
 * ---------------------------------------------------------------------------
 */

extern CMPIBoolean isAbstract(CMPIConstClass * cc);

static int
repCandidate(ClassRegister * cReg, char *cn)
{
  _SFCB_ENTER(TRACE_PROVIDERS, "repCandidate");

  if (strcasecmp(cn, "cim_indicationfilter") == 0 ||
      strcasecmp(cn, "cim_indicationsubscription") == 0) {
    _SFCB_RETURN(0);
  }

  ReadCtl         ctl = tempRead;
  CMPIConstClass *cl = getClass(cReg, cn, &ctl);
  if (isAbstract(cl)) {
    if (ctl != cached)
      CMRelease(cl);
    _SFCB_RETURN(0);
  }
  ProviderInfo   *info;
  int cn_duped = 0;

  while (cn != NULL) {
    info = pReg->ft->getProvider(pReg, cn, INSTANCE_PROVIDER);
    if (info) {
      if (ctl != cached)
	CMRelease(cl);
      _SFCB_RETURN(0);
    }

    if (cn_duped) /* don't free the passed-in cn */
      free(cn);
    cn = (char*) cl->ft->getCharSuperClassName(cl);
    if (cn == NULL)
      break;
    else {
      cn = strdup(cn);
      cn_duped = 1;
    }

    if (ctl != cached)
      CMRelease(cl);
    ctl = tempRead;
    cl = getClass(cReg, cn, &ctl);
  }
  if (ctl != cached)
    CMRelease(cl);
  _SFCB_RETURN(1);
}

static void
loopOnChildChars(ClassRegister * cReg, char *cn, CMPIArray *ar, int *i,
                 int ignprov)
{
  UtilList       *ul = getChildren(cReg, cn);
  char           *child;

  _SFCB_ENTER(TRACE_PROVIDERS, "loopOnChildChars");
  _SFCB_TRACE(1, ("--- class %s", cn));

  if (ul)
    for (child = (char *) ul->ft->getFirst(ul); child;
         child = (char *) ul->ft->getNext(ul)) {
      if (ignprov || repCandidate(cReg, child)) {
        CMSetArrayElementAt(ar, *i, child, CMPI_chars);
        *i = (*i) + 1;
      }
      loopOnChildChars(cReg, child, ar, i, ignprov);
    }
  _SFCB_EXIT();
}

static void
loopOnChildCount(ClassRegister * cReg, char *cn, int *i, int ignprov)
{
  UtilList       *ul = getChildren(cReg, cn);
  char           *child;

  _SFCB_ENTER(TRACE_PROVIDERS, "loopOnChildCount");

  if (ul)
    for (child = (char *) ul->ft->getFirst(ul); child;
         child = (char *) ul->ft->getNext(ul)) {
      if (ignprov || repCandidate(cReg, child))
        *i = (*i) + 1;
      loopOnChildCount(cReg, child, i, ignprov);
    }
  _SFCB_EXIT();
}

/* ClassProviderMethodCleanup */
static CMPIStatus okCleanup(ClassProvider,Method);

static CMPIStatus
ClassProviderInvokeMethod(CMPIMethodMI * mi,
                          const CMPIContext *ctx,
                          const CMPIResult *rslt,
                          const CMPIObjectPath * ref,
                          const char *methodName,
                          const CMPIArgs * in, CMPIArgs * out)
{
  CMPIStatus      st = { CMPI_RC_OK, NULL };
  CMPIArray      *ar;
  int             rc;
  ClassRegister  *cReg;

  _SFCB_ENTER(TRACE_PROVIDERS, "ClassProviderInvokeMethod");

  cReg = getNsReg(ref, &rc);
  if (cReg == NULL) {
    st.rc = CMPI_RC_ERR_INVALID_NAMESPACE;
    semRelease(sfcbSem,INIT_CLASS_PROV_ID);
    _SFCB_RETURN(st);
  }

  if (strcasecmp(methodName, "getchildren") == 0) {
    CMPIData        cn = CMGetArg(in, "class", NULL);
    _SFCB_TRACE(1, ("--- getchildren %s", (char *) cn.value.string->hdl));

    cReg->ft->wLock(cReg);

    if (cn.type == CMPI_string && cn.value.string && cn.value.string->hdl) {
      char           *child;
      int             l = 0,
          i = 0;
      UtilList       *ul =
          getChildren(cReg, (char *) cn.value.string->hdl);
      if (ul)
        l = ul->ft->size(ul);
      ar = CMNewArray(_broker, l, CMPI_string, NULL);
      if (ul)
        for (child = (char *) ul->ft->getFirst(ul); child; child = (char *)
             ul->ft->getNext(ul)) {
          CMSetArrayElementAt(ar, i++, child, CMPI_chars);
        }
      st = CMAddArg(out, "children", &ar, CMPI_stringA);
    } else {
    }

    cReg->ft->wUnLock(cReg);

  }

  else if (strcasecmp(methodName, "getallchildren") == 0) {
    int             ignprov = 0;
    CMPIData        cn = CMGetArg(in, "class", &st);

    cReg->ft->wLock(cReg);

    if (st.rc != CMPI_RC_OK) {
      cn = CMGetArg(in, "classignoreprov", NULL);
      ignprov = 1;
    }
    _SFCB_TRACE(1,
                ("--- getallchildren %s", (char *) cn.value.string->hdl));
    if (cn.type == CMPI_string && cn.value.string && cn.value.string->hdl) {
      int             n = 0,
          i = 0;
      loopOnChildCount(cReg, (char *) cn.value.string->hdl, &n, ignprov);
      _SFCB_TRACE(1, ("--- count %d", n));
      ar = CMNewArray(_broker, n, CMPI_string, NULL);
      if (n) {
        _SFCB_TRACE(1, ("--- loop %s", (char *) cn.value.string->hdl));
        loopOnChildChars(cReg, (char *) cn.value.string->hdl, ar, &i,
                         ignprov);
      }
      st = CMAddArg(out, "children", &ar, CMPI_stringA);
    } else {
    }

    cReg->ft->wUnLock(cReg);
  }

  else if (strcasecmp(methodName, "getassocs") == 0) {
    ar = CMNewArray(_broker, cReg->topAssocs, CMPI_string, NULL);
    ClassBase      *cb = (ClassBase *) (cReg + 1);
    UtilHashTable  *ct = cb->ht;
    HashTableIterator *i;
    char           *cn;
    ClassRecord    *crec;
    int             n;

    cReg->ft->wLock(cReg);

    for (n = 0, i = ct->ft->getFirst(ct, (void **) &cn, (void **) &crec);
         i; i = ct->ft->getNext(ct, i, (void **) &cn, (void **) &crec)) {
      if (crec->flags & CREC_isAssociation && crec->parent == NULL) {
        /*
         * add top-level association class 
         */
        CMSetArrayElementAt(ar, n++, cn, CMPI_chars);
      }
    }
    CMAddArg(out, "assocs", &ar, CMPI_stringA);

    cReg->ft->wUnLock(cReg);
  }

  else if (strcasecmp(methodName, "ischild") == 0) {
    char           *parent = (char *) CMGetClassName(ref, NULL)->hdl;
    char           *chldn =
        (char *) CMGetArg(in, "child", NULL).value.string->hdl;
    st.rc = traverseChildren(cReg, parent, chldn);
  }

  else if (strcasecmp(methodName, "listnamespaces") == 0) {

    HashTableIterator *hit;
    char           *key;
    ClassRegister  *cReg;

    CMPIArray* ar = CMNewArray(_broker, nsHt->ft->size(nsHt), CMPI_string, NULL);
    int i = 0;

    /* req for specific ns */
    CMPIData nsd = CMGetArg(in, "ns", &st);
    if (st.rc == CMPI_RC_OK) {
      char* ns = CMGetCharPtr(nsd.value.string);
        ClassRegister  *cReg = NULL;
        cReg = nsHt->ft->get(nsHt, ns);
        st.rc = (cReg) ? CMPI_RC_OK : CMPI_RC_ERR_NOT_FOUND;
    }
    else {
      for (hit = nsHt->ft->getFirst(nsHt, (void **) &key, (void **) &cReg);
           key && hit;
           hit =
             nsHt->ft->getNext(nsHt, hit, (void **) &key, (void **) &cReg)) {

        CMSetArrayElementAt(ar, i++, key, CMPI_chars);
      }
  
      CMAddArg(out, "nslist", &ar, CMPI_stringA);
      st.rc = CMPI_RC_OK;
    }

  }

  else if (strcasecmp(methodName, "_startup") == 0) {

    /* let providerMgr know that we're done init'ing  */
    semRelease(sfcbSem,INIT_CLASS_PROV_ID);

    st.rc = CMPI_RC_OK;
  }

  else {
    mlogf(M_ERROR, M_SHOW,
          "--- ClassProvider: Invalid invokeMethod request %s\n",
          methodName);
    st.rc = CMPI_RC_ERR_METHOD_NOT_FOUND;
  }
  _SFCB_RETURN(st);
}

int
traverseChildren(ClassRegister * cReg, const char *parent,
                 const char *chldn)
{
  char           *child;
  int             rc = CMPI_RC_ERR_FAILED;
  UtilList       *ul = getChildren(cReg, parent);

  cReg->ft->rLock(cReg);

  if (ul)
    for (child = (char *) ul->ft->getFirst(ul); child;
         child = (char *) ul->ft->getNext(ul)) {
      if (strcasecmp(child, chldn) == 0) {
        rc = CMPI_RC_OK;
        break;
      } else {
        rc = traverseChildren(cReg, child, chldn);
        if (rc == CMPI_RC_OK)
          break;
      }
    }

  cReg->ft->rUnLock(cReg);
  return rc;
}

CMClassMIStub(ClassProvider, ClassProvider, _broker, initialize(&mi, ctx));

CMMethodMIStub(ClassProvider, ClassProvider, _broker, CMNoHook);

// 
// 
/* MODELINES */
/* DO NOT EDIT BELOW THIS COMMENT */
/* Modelines are added by 'make pretty' */
/* -*- Mode: C; c-basic-offset: 2; indent-tabs-mode: nil; -*- */
/* vi:set ts=2 sts=2 sw=2 expandtab: */
