#!/bin/bash
rm -f ${OABI_TC:-/opt/oabi-tc}/PATHB_DONE
podman run --rm --name oabi-pathb -v ${OABI_TC:-/opt/oabi-tc}:/work oabi-build:lenny \
   /bin/bash /work/kegel/run_pathb.sh >${OABI_TC:-/opt/oabi-tc}/pathb.log 2>&1
echo "$?" > ${OABI_TC:-/opt/oabi-tc}/PATHB_DONE
