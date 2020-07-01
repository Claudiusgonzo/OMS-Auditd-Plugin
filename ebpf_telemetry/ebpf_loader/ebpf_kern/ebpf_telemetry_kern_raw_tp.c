/*
    microsoft-oms-auditd-plugin

    Copyright (c) Microsoft Corporation

    All rights reserved.

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/


#include "ebpf_kern_common.h"

// struct bpf_raw_tracepoint_args {
// 	__u64 args[0];
// };

SEC("raw_tracepoint/sys_enter")
int sys_enter(struct bpf_raw_tracepoint_args *ctx)
{
    u64 pid_tid = bpf_get_current_pid_tgid();
    u32 map_id = 0;
    event_s *event = NULL;
    u32 config_id = 0;
    config_s *config;
    u32 userland_pid = 0;

    // bail early for syscalls we aren't interested in
    unsigned long long syscall = ctx->args[1];
    if ( (syscall != __NR_open)    &&
         (syscall != __NR_openat)  && 
         (syscall != __NR_execve)  &&
         (syscall != __NR_accept)  && 
         (syscall != __NR_accept4) && 
         (syscall != __NR_connect) )
        return 0;

    // retrieve config
    config = bpf_map_lookup_elem(&config_map, &config_id);
    if (!config)
        return 0;

    userland_pid = config->pid;

    if ((pid_tid >> 32) == userland_pid)
        return 0;

    // retrieve map storage for event
    event = bpf_map_lookup_elem(&event_storage_map, &map_id);
    if (!event)
        return 0;

    event->code_bytes = CODE_BYTES;
    event->version = VERSION;
    event->syscall_id = ctx->args[1];
    event->pid = pid_tid >> 32;

    volatile struct pt_regs *regs = (struct pt_regs *)ctx->args[0];
    
    // arch/ABI      arg1  arg2  arg3  arg4  arg5  arg6  arg7  
    // ------------------------------------------------------
    // x86-64        rdi   rsi   rdx   r10   r8    r9    -

    switch(event->syscall_id)
    {
        // int openat(int dirfd, const char *pathname, int flags);
        // int openat(int dirfd, const char *pathname, int flags, mode_t mode);
        case __NR_open:
        case __NR_openat: // syscall id #s might be kernel specific
        {
            volatile const char *pathname;

            bpf_probe_read(&pathname, sizeof(pathname), (void *)&PT_REGS_PARM2(regs)); //read addr into char*
            bpf_probe_read_str(event->data.openat.filename, sizeof(event->data.openat.filename), (void *)pathname); // read str from char*
            
            // Debug
            // char fmt[] = "OPEN: %s\n";
            // bpf_trace_printk(fmt, sizeof(fmt), event->data.openat.filename);
            
            break;
        }
        
        // int execve(const char *filename, char *const argv[], char *const envp[]);
        case __NR_execve: 
        {
            const char **argv; 
            volatile const char *filename;
	        volatile const char *argp;
            unsigned int ret;
           
            // get filename 
            bpf_probe_read(&filename, sizeof(filename), (void *)&PT_REGS_PARM1(regs)); //read addr into char*
            bpf_probe_read_str(event->data.execve.exe, sizeof(event->data.execve.exe), (void *)filename); // read str from char*
            
            // get the args
            bpf_probe_read(&argv, sizeof(argv), (void *)&PT_REGS_PARM2(regs)); // read argv[]
            
            event->data.execve.args_count = 0;
            event->data.execve.args_size  = 0;
            #pragma unroll // no loops in eBPF, but need to get all args....
            for (int i = 1; i < TOTAL_MAX_ARGS; i++) {
                bpf_probe_read(&argp, sizeof(argp), (void*) &argv[i]);
                if (!argp)
                    break;

                // This is important or the verifier will reject without a bounds check
                if (event->data.execve.args_size > ARGSIZE )
                    break;
                
                ret = bpf_probe_read_str(&event->data.execve.cmdline[event->data.execve.args_size], ARGSIZE, (void*) argp);
                if (ret > ARGSIZE)
                    break;

                if ( ret > 0 ){ 
                    event->data.execve.args_count++;
                    event->data.execve.args_size += ret;
                }
            }
            
            break;
        }
        
        // int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
        case __NR_connect: 
        {
            struct sockaddr *addr;
                        
            bpf_probe_read(&addr, sizeof(addr), (void *)&PT_REGS_PARM2(regs));
            bpf_probe_read(&event->data.socket.addr, sizeof(event->data.socket.addr), (void *)addr);
        }

        case __NR_accept4:
        case __NR_accept:
            bpf_probe_read(&event->data.socket.addrp, sizeof(event->data.socket.addrp), (void *)&regs->si);
            
            // if (NULL == event->data.socket.addrp){
            //     char fmt[] = "ACCEPT: Empty\n";
            //     bpf_trace_printk(fmt, sizeof(fmt));
            // }

            break;
        
    }

    // store event in the hash
    bpf_map_update_elem(&events_hash, &pid_tid, event, BPF_ANY);

    return 0;
}

SEC("raw_tracepoint/sys_exit")
int sys_exit(struct bpf_raw_tracepoint_args *ctx)
{
    u64 pid_tid = bpf_get_current_pid_tgid();
    u32 map_id = 0;
    event_s *event = NULL;
    volatile struct pt_regs *regs = (struct pt_regs *)ctx->args[0];
    u32 config_id = 0;
    config_s *config;
    u32 userland_pid = 0;
    
    // retrieve config
    config = bpf_map_lookup_elem(&config_map, &config_id);
    if (!config)
        return 0;

    userland_pid = config->pid;

    if ((pid_tid >> 32) == userland_pid)
        return 0;

    // retrieve map storage for event
    // this was mostly completed on the preceding sys_enter
    // if the pid_tid is in our map then we must have stored it
    event = bpf_map_lookup_elem(&events_hash, &pid_tid);
    if (!event)
        // otherwise bail
        return 0;

    //event->return_code = ctx->ret;
    bpf_probe_read(&event->return_code, sizeof(s64), (void *)&PT_REGS_RC(regs));

    switch(event->syscall_id)
    {
        case __NR_accept4:
        case __NR_accept:
        {
            bpf_probe_read(&event->data.socket.addr, 
                           sizeof(event->data.socket.addr), 
                           (void *)event->data.socket.addrp);
            break;
        }
    }

    bpf_perf_event_output(ctx, &event_map, BPF_F_CURRENT_CPU, event, sizeof(event_s));
    bpf_map_delete_elem(&events_hash, &pid_tid);

    return 0;
}

char _license[] SEC("license") = "GPL";
