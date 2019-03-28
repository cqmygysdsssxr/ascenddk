#!/bin/bash
#
#   =======================================================================
#
# Copyright (C) 2018, Hisilicon Technologies Co., Ltd. All Rights Reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are met:
#
#   1 Redistributions of source code must retain the above copyright notice,
#     this list of conditions and the following disclaimer.
#
#   2 Redistributions in binary form must reproduce the above copyright notice,
#     this list of conditions and the following disclaimer in the documentation
#     and/or other materials provided with the distribution.
#
#   3 Neither the names of the copyright holders nor the names of the
#   contributors may be used to endorse or promote products derived from this
#   software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
# AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
# ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
# LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
# CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
# SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
# INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
# CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
# ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
# POSSIBILITY OF SUCH DAMAGE.
#   =======================================================================

# ************************Variable*********************************************

script_path="$( cd "$(dirname "$0")" ; pwd -P )"

remote_host=$1
download_mode=$2

common_path="${script_path}/.."

. ${common_path}/utils/scripts/func_deploy.sh
. ${common_path}/utils/scripts/func_util.sh

function deploy()
{
    libs=$1
    for lib_name in ${libs}
    do
        echo "${host_libraries[@]}" | grep "${lib_name}" 1>/dev/null
        if [ $? -eq 0 ];then
            upload_file "${HOST_LIB_PATH}/${lib_name}" "~/HIAI_PROJECTS/ascend_lib"
            if [ $? -ne 0 ];then
                return 1
            fi
        fi

        echo "${device_libraries[@]}" | grep "${lib_name}" 1>/dev/null
        if [ $? -eq 0 ];then
            upload_file "${DEVICE_LIB_PATH}/${lib_name}" "~/HIAI_PROJECTS/ascend_lib"
            if [ $? -ne 0 ];then
                return 1
            fi
        fi
    done
    echo "Finish to upload libs."
    return 0
}

main()
{
    check_ip_addr ${remote_host}
    if [[ $? -ne 0 ]];then
        echo "ERROR: invalid host ip, please check your command format: ./deploy.sh host_ip [download_mode(local/internet)]."
        exit 1
    fi
    
    echo "[Step] Build common libs..."
    bash ${common_path}/build.sh
    if [[ $? -ne 0 ]];then
        exit 1
    fi
    echo "[Step] Build ascendcamera..."
    make clean -C ${script_path} 1>/dev/null
    if [[ $? -ne 0 ]];then
        exit 1
    fi
    make -C ${script_path} 1>/dev/null
    if [[ $? -ne 0 ]];then
        exit 1
    fi
    
    #parse remote port
    parse_remote_port
    
    echo "[Step] Deploy common libs..."
    bash ${common_path}/deploy.sh ${remote_host}
    if [[ $? -ne 0 ]];then
        exit 1
    fi
    
    echo "[Step] Deploy ascendcamera"
    upload_file "${script_path}/out/ascendcamera" "~/HIAI_PROJECTS/ascend_workspace/ascendcamera/out"
    if [[ $? -ne 0 ]];then
        exit 1
    fi
    iRet=`IDE-daemon-client --host ${remote_host}:${remote_port} --hostcmd "chmod +x ~/HIAI_PROJECTS/ascend_workspace/ascendcamera/out/ascendcamera"`
    if [[ $? -ne 0 ]];then
        echo "ERROR: change excution mode ${remote_host}:./HIAI_PROJECTS/ascend_workspace/${app_name}/out/* failed, please check /var/log/syslog for details."
        return 1
    fi
    
    echo "[Step] Prepare presenter server information..."
    bash ${common_path}/prepare_presenter_server.sh "display" ${remote_host} ${download_mode}
    if [[ $? -ne 0 ]];then
        exit 1
    fi
    exit 0
}

main
