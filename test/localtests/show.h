#include "CimClientLib/cmci.h"
#include "CimClientLib/native.h"
#include <unistd.h>

extern char    *value2Chars(CMPIType type, CMPIValue * value);

void            showObjectPath(CMPIObjectPath * objectpath);
void            showInstance(CMPIInstance *instance);
void            showClass(CMPIConstClass * in_class);
void            showProperty(CMPIData data, char *name);
/* MODELINES */
/* DO NOT EDIT BELOW THIS COMMENT */
/* Modelines are added by 'make pretty' */
/* -*- Mode: C; c-basic-offset: 2; indent-tabs-mode: nil; -*- */
/* vi:set ts=2 sts=2 sw=2 expandtab: */
