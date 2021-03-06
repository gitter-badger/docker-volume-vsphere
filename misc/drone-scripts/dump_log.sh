#!/bin/sh
# Copyright 2016 VMware, Inc. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#    http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# A few helper functions for dumping logs


SSH="ssh -o StrictHostKeyChecking=no"
USER=root

VM_LOGFILE="/var/log/docker-volume-vsphere.log"
VM_STDLOG="/tmp/plugin.log"
ESX_LOGFILE="/var/log/vmware/vmdk_ops.log"

dump_log_esx() {
  echo ""
  echo " ESX Config info for $1 ***************"
  cmd='vmware -l; uname -a; df; ls -ld /vmfs/volumes/*'
  echo $cmd ; $SSH $USER@$1 $cmd

  echo "*** dumping log: ESX $1"
  echo "*************************************************************************"
  set -x
  $SSH $USER@$1 cat $ESX_LOGFILE
  set +x
  echo "*************************************************************************"
}

dump_log_vm(){
  echo ""
  echo "*** dumping log: VM $1"
  echo "*************************************************************************"
  set -x
  $SSH $USER@$1 cat $VM_STDLOG
  $SSH $USER@$1 cat $VM_LOGFILE
  set +x
  echo "*************************************************************************"
}

# dump log vm1 vm2 esx
dump_log() { 
  dump_log_esx $3
  dump_log_vm $1
  dump_log_vm $2
}
