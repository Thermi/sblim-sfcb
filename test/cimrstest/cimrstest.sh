#!/bin/bash
# ============================================================================
# xmltest
#
# (C) Copyright IBM Corp. 2005
#
# THIS FILE IS PROVIDED UNDER THE TERMS OF THE ECLIPSE PUBLIC LICENSE
# ("AGREEMENT"). ANY USE, REPRODUCTION OR DISTRIBUTION OF THIS FILE
# CONSTITUTES RECIPIENTS ACCEPTANCE OF THE AGREEMENT.
#
# You can obtain a current copy of the Eclipse Public License from
# http://www.opensource.org/licenses/eclipse-1.0.php
#
# Author:       Dr. Gareth S. Bestor, <bestorga@us.ibm.com>
# Contributors: Adrian Schuur, <schuur@de.ibm.com>
# Description:
#    Simple test program to send CIM-XML test request files to a CIMOM and
#    compare the returned results to an expected response file.
#    If test file is specified then run only that test. If test directory is
#    specified then run all tests in the directory in sorted order. If no test
#    file or directory specified then run all tests in the current directory.
# ============================================================================

_RC=0
TESTDIR=.

#TODO: Remove!
SFCB_TEST_PORT=5988
SFCB_TEST_PROTOCOL=http

# Check for wbemcat utility
if ! which wbemcat > /dev/null; then
   echo "  Cannot find wbemcat. Please check your PATH"
   exit 1
fi
if ! touch ./testfile > /dev/null; then
   echo "  Cannot create files, check permissions"
   exit 1
fi
rm -f ./testfile

#TODO: will need to expand for POST
for reqfile in `ls *GET`
do
   _TEST=${reqfile%.GET}
   _TESTOK=$_TEST.OK
   _TESTLINES=$_TEST.lines
   _TESTRESULT=$_TEST.result
   _TESTPREREQ=$_TEST.prereq
   _TESTNAME=$_TEST

   echo -n "  Testing $_TESTNAME..."
   # Check if there's a .prereq file and if so, run it. Skip this
   # test if it returns "1"
   if [ ! -f $_TESTPREREQ ] ||  eval ./$_TESTPREREQ  ; then

       # Remove any old test result file
       rm -f $_TESTRESULT

       #read reqfile; pull out the path
       declare -a ARRAY
       exec 10<$reqfile        #open file
       j=0
       while read LINE <&10; do
	   ARRAY[$j]=$LINE
	   ((j++))
       done
       exec 10<&-       # close file

       path=${ARRAY[0]}
       ARRAY[0]=""

       path=`head -n 1 ${reqfile}`

       # Send request to CIMOM

       if [ "$SFCB_TEST_USER" != "" ] && [ "$SFCB_TEST_PASSWORD" != "" ]; then
           wbemcat -u $SFCB_TEST_USER -pwd $SFCB_TEST_PASSWORD -p $SFCB_TEST_PORT -t $SFCB_TEST_PROTOCOL --type=application/json --method=GET --path=$path > $_TESTRESULT <<EOF
${ARRAY[@]}
EOF
       else
           wbemcat -p $SFCB_TEST_PORT -t $SFCB_TEST_PROTOCOL --type=application/json --method=GET --path=$path > $_TESTRESULT <<EOF
${ARRAY[@]}
EOF

       fi

       if [ $? -ne 0 ]; then
          echo "FAILED to send CIM-RS request"
          _RC=1
          continue
       fi
    
       # Compare the response XML against the expected XML for differences
       # Either using a full copy of the expected output (testname.OK)
       if [ -f $_TESTOK ] ; then
            if ! diff --brief $_TESTOK $_TESTRESULT > /dev/null; then
                echo "FAILED output not as expected"
                _RC=1;
                continue
    
            # We got the expected response XML
            else
                echo "PASSED"
                rm -f $_TESTRESULT
            fi
       fi
       # or a file containing individual lines that must be found (testname.lines)
       if [ -f $_TESTLINES ] ; then
            passed=0
            rm -f ./tmpfail
            while read line 
            do
              # Check for disallowed lines (line starts with "!" in .lines file)
              notline=$(echo $line | awk '{ line=index($line,"!"); print line; }' )
              if [ "$notline" != 0 ] ; then
                text=$(echo $line | awk '{ line=substr($line, 2); print line; }' )
                if  grep --q -F "$text" $_TESTRESULT  ; then
                    if [ $passed -eq 0 ] ; then
                        echo "FAILED disallowed line found"
                        passed=1
                        _RC=1;
                    fi
                    echo "FAILED disallowed line found" >> ./tmpfail
                    echo "\t$text" >> ./tmpfail
                fi
              else
                # Check for required lines
                if ! grep --q -F "$line" $_TESTRESULT  ; then
                    if [ $passed -eq 0 ] ; then
                        echo "FAILED required line not found"
                        passed=1
                        _RC=1;
                    fi
                    echo "FAILED: required line not found" >> ./tmpfail
                    echo "\t$line" >> ./tmpfail
                fi
              fi
            done < $_TESTLINES
            if [ -f ./tmpfail ]; then
                    echo "\n\n**** Test Failed ****\n\n" >> $_TESTRESULT
                    cat ./tmpfail >> $_TESTRESULT
                    rm -f ./tmpfail
            fi
    
            if [ $passed -eq 0 ] ;  then
                echo "PASSED"
                rm -f $_TESTRESULT
            fi
                
       fi
   else 
        # Prereq test failed, so skip this test.
        echo "SKIPPED prerequisite not met"
   fi
done
## Staged test, ignore RC. Remove this when implemented
echo "Test is staged, ignoring failures."
_RC=0
##

exit $_RC

