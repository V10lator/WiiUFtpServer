/*

Copyright (C) 2008 Joseph Jordan <joe.ftpii@psychlaw.com.au>

This software is provided 'as-is', without any express or implied warranty.
In no event will the authors be held liable for any damages arising from
the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1.The origin of this software must not be misrepresented; you must not
claim that you wrote the original software. If you use this software in a
product, an acknowledgment in the product documentation would be
appreciated but is not required.

2.Altered source versions must be plainly marked as such, and must not be
misrepresented as being the original software.

3.This notice may not be removed or altered from any source distribution.

*/
/****************************************************************************
  * WiiUFtpServer
  * 2021-12-05:Laf111:V7-0
 ***************************************************************************/
 
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <malloc.h>
#include <unistd.h>

#include <whb/log.h>
#include <whb/log_console.h>

#include <errno.h>
#include <sys/fcntl.h>
#include <nsysnet/_socket.h>
#include <coreinit/memdefaultheap.h>

#include <nn/ac.h>

#include <iosuhax.h>

#include "vrt.h"
#include "ftp.h"
#include "net.h"

#define SOCKET_MOPT_STACK_SIZE 0x2000
extern int somemopt (int req_type, char* mem, unsigned int memlen, int flags);

extern void display(const char *fmt, ...);

static uint32_t nbFilesDL = 0;
static uint32_t nbFilesUL = 0;

#ifdef LOG2FILE
    extern void writeToLog(const char *fmt, ...);
#endif

// IOSUHAX fd
static int fsaFd = -1;

static uint32_t hostIpAddress = 0;

static const int retriesNumber = (int) ((float)(FTP_CONNECTION_TIMEOUT) / ((float)NET_RETRY_TIME_STEP_MILLISECS/1000.0));

static OSThread socketOptThread;
static uint8_t *socketOptThreadStack=NULL;

static bool initDone = false;

int socketOptThreadMain(int argc UNUSED, const char **argv UNUSED)
{
    void *buf = MEMAllocFromDefaultHeapEx(SOMEMOPT_BUFFER_SIZE, 64);
    if (buf == NULL)
    {
        display("! ERROR : Socket optimizer: OUT OF MEMORY!");
        return -ENOMEM;
    }

    if (somemopt(0x01, buf, SOMEMOPT_BUFFER_SIZE, 0) == -1 && errno != 50)
    {if (!initDone) display("! ERROR : somemopt failed !");}

    MEMFreeToDefaultHeap(buf);

    return 0;
}


static bool retry(int32_t socketError) {
    bool status = false;
    
    // retry
    if (socketError == -EINPROGRESS ||
        socketError == -EALREADY ||        
        socketError == -EBUSY ||
        socketError == -ETIME ||
        socketError == -ECONNREFUSED ||
        socketError == -ECONNABORTED ||
        socketError == -ETIMEDOUT ||
        socketError == -EMFILE ||
        socketError == -ENFILE ||
        socketError == -EHOSTUNREACH ||
        socketError == -EISCONN) status = true;

    #ifdef LOG2FILE    
        if (status) display("~ WARNING : Retrying transfer after = %d (%s)", socketError, strerror(socketError));
    #endif
    return status;
}

int32_t initialize_network()
{
    unsigned int nn_startupid;

    ACInitialize();
    ACGetStartupId(&nn_startupid);
    ACConnectWithConfigId(nn_startupid);
    ACGetAssignedAddress(&hostIpAddress);

    socketOptThreadStack = MEMAllocFromDefaultHeapEx(SOCKET_MOPT_STACK_SIZE, 8);
    
    if (socketOptThreadStack == NULL || !OSCreateThread(&socketOptThread, socketOptThreadMain, 0, NULL, socketOptThreadStack + SOCKET_MOPT_STACK_SIZE, SOCKET_MOPT_STACK_SIZE, FTP_NB_SIMULTANEOUS_TRANSFERS+2, OS_THREAD_ATTRIB_AFFINITY_CPU0)) {
        display("! ERROR : failed to create socket memory optimization thread!");
        return -1;
    }
    OSSetThreadName(&socketOptThread, "Socket memory optimizer thread");
    OSResumeThread(&socketOptThread);

    #ifdef LOG2FILE    
        writeToLog("socketThreadMain ready");
    #endif
        
    #ifdef LOG2FILE      
        display("--------------- MAIN SETTINGS ----------------");
        display("NB_SIMULTANEOUS_TRANSFERS     = %d", NB_SIMULTANEOUS_TRANSFERS);
        display("NET_RETRY_TIME_STEP_MILLISECS = %d", NET_RETRY_TIME_STEP_MILLISECS);
        display("network retries number        = %d", retriesNumber);
        display("SOCKET_BUFFER_SIZE            = %d", SOCKET_BUFFER_SIZE);
        display("SND_BUFFER_SIZE               = %d", SND_BUFFER_SIZE);
        display("RCV_BUFFER_SIZE               = %d", RCV_BUFFER_SIZE);
        display("TRANSFER_BUFFER_SIZE          = %d", TRANSFER_BUFFER_SIZE);
        display("FTP_CONNECTION_TIMEOUT        = %d", FTP_CONNECTION_TIMEOUT);
        display("----------------------------------------------");
   #endif
   
   return 0;
}

void finalize_network()
{
    display("  Files received : %d", nbFilesUL);
    display("  Files sent     : %d", nbFilesDL);
    display("------------------------------------------------------------");
    
    if (socketOptThreadStack != NULL) socket_lib_finish();
    int ret;
    OSJoinThread(&socketOptThread, &ret);
    
    if (socketOptThreadStack != NULL) MEMFreeToDefaultHeap(socketOptThreadStack);

}

int32_t network_socket(uint32_t domain,uint32_t type,uint32_t protocol)
{
    
    int s = socket(domain, type, protocol);
    if(s < 0)
    {
        int err = -errno;
        return (err < 0) ? err : s;
    }
    
    if (type == SOCK_STREAM)
    {
        int enable = 1;
		
		// Reuse socket
	    if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable))!=0) 
	        {if (!initDone) display("! ERROR : setsockopt / SO_REUSEADDR failed !");}        

        // SO_NOSLOWSTART
        if (setsockopt(s, SOL_SOCKET, SO_NOSLOWSTART, &enable, sizeof(enable))!=0) 
            {if (!initDone) display("! ERROR : setsockopt / Disable slow start feature failed !");}    
        
        // Activate TCP SAck
        if (setsockopt(s, SOL_SOCKET, SO_TCPSACK, &enable, sizeof(enable))!=0) 
            {if (!initDone) display("! ERROR : setsockopt / TCP SAck activation failed !");}
                
        // Activate WinScale
        if (setsockopt(s, SOL_SOCKET, SO_WINSCALE, &enable, sizeof(enable))!=0) 
            {display("! ERROR : setsockopt / winScale activation failed !");}
        
        // socket memory optimization
        int nbTries=0;
        try_again_someopt:
        if (setsockopt(s, SOL_SOCKET, SO_RUSRBUF, &enable, sizeof(enable))!=0) 
        {
            OSSleepTicks(OSMillisecondsToTicks(NET_RETRY_TIME_STEP_MILLISECS));
            nbTries++;
            if (nbTries <= retriesNumber) goto try_again_someopt;
            else if (!initDone) display("! ERROR : setsockopt / Socket memory optimization failed !");
        }
        
        // Set to non-blocking I/O
        set_blocking(s, false); 
        
        if (!initDone) {
            initDone = true;
            display("  1 client only using a max of %d slots for up/download!", NB_SIMULTANEOUS_TRANSFERS);
            display("   (set your client's timeout greater than %d seconds)", FTP_CONNECTION_TIMEOUT+5);
        }
	}
    return s;
}


int32_t network_bind(int32_t s,struct sockaddr *name,int32_t namelen)
{
    int res = bind(s, name, namelen);
    if (res < 0)
    {
        int err = -errno;
        return (err < 0) ? err : res;
    }
    return res;
}

int32_t network_listen(int32_t s,uint32_t backlog)
{
    
    int res = listen(s, backlog);
    if (res < 0)
    {
        int err = -errno;
        return (err < 0) ? err : res;
    }
    return res;
}

int32_t network_accept(int32_t s,struct sockaddr *addr,int32_t *addrlen)
{
    socklen_t addrl=(socklen_t) *addrlen;
    
    
    int res = accept(s, addr, &addrl);
    if (res < 0)
    {
        int err = -errno;
        return (err < 0) ? err : res;
    }
    
    
    return res;
}

int32_t network_connect(int32_t s,struct sockaddr *addr, int32_t addrlen)
{
    
    int res = connect(s, addr, addrlen);
    if (res < 0)
    {
        int err = -errno;
        return (err < 0) ? err : res;
    }
    return res;
}

int32_t network_read(int32_t s,void *mem,int32_t len)
{
    int res = recv(s, mem, len, 0);
    if (res < 0)
    {
        int err = -errno;
        return (err < 0) ? err : res;
    }
    return res;
}

static int32_t network_readChunk(int32_t s,void *mem,int32_t len) {

    int32_t received = 0;
    int ret = -1;
    
    
    // while buffer is not full (len >0)
    while (len > 1)
    {
        ret = recv(s, mem, len-1, 0);        
        if (ret == 0) {
            // client EOF detected
            break;
        } else if (ret < 0 && errno != EAGAIN) {
            int err = -errno;
            received = (err < 0) ? err : ret;
            break;
        } else {
            if (ret > 0) {
                received += ret;
                len -= ret;
                mem += ret;
            }
        }
    }
    return received;
}

uint32_t network_gethostip()
{
    return hostIpAddress;
}

int32_t network_write(int32_t s, const void *mem, int32_t len)
{
    int32_t transfered = 0;
    
    while (len)
    {
        int ret = send(s, mem, len, 0);
        if (ret < 0 && errno != EAGAIN)
        {
            int err = -errno;
            transfered = (err < 0) ? err : ret;
            break;
        } else {
            if (ret > 0) {
                mem += ret;
                transfered += ret;
                len -= ret;
            }
        }
    }
    return transfered;
}

int32_t network_close(int32_t s)
{
    if (s < 0)
        return -1;
    return close(s);
}

int32_t set_blocking(int32_t s, bool blocking) {
    int32_t block = !blocking;
    setsockopt(s, SOL_SOCKET, SO_NONBLOCK, &block, sizeof(block));
    return 0;
}

int32_t network_close_blocking(int32_t s) {
    set_blocking(s, true);
    return network_close(s);
}

int32_t send_exact(int32_t s, char *buf, int32_t length) {
    int buf_size = length;
    int32_t result = 0;
    int32_t remaining = length;
    int32_t bytes_transfered;
    
    uint32_t retryNumber = 0;

    set_blocking(s, true);
    while (remaining) {

        retry:
        bytes_transfered = network_write(s, buf, MIN(remaining, (int) buf_size));
        
        if (bytes_transfered > 0) {
            remaining -= bytes_transfered;
            buf += bytes_transfered;
        } else if (bytes_transfered < 0) {

            if (retry(bytes_transfered)) {
                OSSleepTicks(OSMillisecondsToTicks(NET_RETRY_TIME_STEP_MILLISECS));
                retryNumber++;
                if (retryNumber <= retriesNumber) goto retry;
            }
            
    #ifdef LOG2FILE 
            display("! ERROR : network_write failed = %d afer %d attempts", bytes_transfered, retriesNumber);
            display("! ERROR : errno = %d (%s)", errno, strerror(errno));            
    #endif
            result = bytes_transfered;
            break;
        } else {
            // result = bytes_transfered = 0
            result = bytes_transfered;                            
            break;
        }

    }
    set_blocking(s, false);
    
    return result;
}

static void sockOptForUpload(int32_t s)
{
    int enable = 1;
                
    // TCP_NODELAY 
    if (setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &enable, sizeof(enable))!=0) 
        {if (!initDone) display("! ERROR : setsockopt / Disabling Nagle's algorithm failed !");}

    // Disable delayed ACKs
    if (setsockopt(s, IPPROTO_TCP, TCP_NOACKDELAY, &enable, sizeof(enable))!=0)
        {if (!initDone) display("! ERROR : setsockopt / Disabling delayed ACKs failed !");}
        
}


int32_t send_from_file(int32_t s, connection_t* connection) {
    // return code
    int32_t result = 0;
        
    #ifdef LOG2FILE    
        writeToLog("C[%d] sending %d bytes of %s on socket %d", connection->index+1, 2*SOCKET_BUFFER_SIZE, connection->fileName,s);
    #endif

    // max value for SNDBUF = SOCKET_BUFFER_SIZE (the system double the value set)
    int sndBuffSize = SND_BUFFER_SIZE;

    if (setsockopt(s, SOL_SOCKET, SO_SNDBUF, &sndBuffSize, sizeof(sndBuffSize))!=0)
        {display("! ERROR : setsockopt / SNDBUF failed !");
    }
    
    int fd = fileno(connection->f);
    // current pos in file should be connection->restart_marker;    
    if (connection->restart_marker && lseek(fd, connection->restart_marker, SEEK_SET) != connection->restart_marker) {
        connection->restart_marker = 0;
    }
    
	int32_t bytes_read = TRANSFER_BUFFER_SIZE;        
	while (bytes_read) {
        bytes_read = fread(connection->transferBuffer, 1, TRANSFER_BUFFER_SIZE, connection->f);
        if (bytes_read == 0) {
            // SUCCESS, no more to write to file                    
            nbFilesDL++;
            result = 0;
            break;
        }        
        if (bytes_read > 0) {
            
            uint32_t retryNumber = 0;
            int32_t remaining = bytes_read;            
                
            // to let buffer on file be larger than socket one
            while (remaining) {
                
                send_again:
                // lower the load on the network on file with size > TRANSFER_BUFFER_SIZE
                result = network_write(s, connection->transferBuffer, MIN(remaining, TRANSFER_BUFFER_SIZE));

                #ifdef LOG2FILE    
                    writeToLog("C[%d] sent %d bytes of %s", connection->index+1, result, connection->fileName);
                #endif
                
                if (result < 0) {
                    if (retry(result)) {
                        OSSleepTicks(OSMillisecondsToTicks(NET_RETRY_TIME_STEP_MILLISECS));
                        retryNumber++;
                        if (retryNumber <= retriesNumber) goto send_again;
                    }            
                    // result = error, connection will be closed
                    break;
                } else {
                    // data block sent sucessfully, continue
                    connection->dataTransferOffset += result;
                    connection->bytesTransfered = result;
                    remaining -= result;
                }
            }        
		}
        if (result >=0) {
                
            // check bytes read (now because on the last sending, data is already sent here = result)
            if (bytes_read < TRANSFER_BUFFER_SIZE) {
                    
            	if (bytes_read < 0 || feof(connection->f) == 0 || ferror(connection->f) != 0) {
                    // ERROR : not on eof file or read error, or error on stream => ERROR
                    display("! ERROR : failed to read file!");
                    display("! ERROR : fread = %d and bytes = %d", bytes_read, TRANSFER_BUFFER_SIZE);
                    display("! ERROR : errno = %d (%s)", errno, strerror(errno)); 
                    result = -103;
                    break;
                }
            }
            
            // result = 0 and EOF
            if ((feof(connection->f) != 0) && (result == 0)) {
                // SUCESS : eof file, last data bloc sent
                nbFilesDL++;
                break;
            }
        }
    }
    
    connection->bytesTransfered = result;

    return result;
}


int32_t recv_to_file(int32_t s, connection_t* connection) {
    // return code
    int32_t result = 0;
    
    // extra socket optimizations
    sockOptForUpload(s);

    // (the system double the value set)
    int rcvBuffSize = RCV_BUFFER_SIZE;
    if (setsockopt(s, SOL_SOCKET, SO_RCVBUF, &rcvBuffSize, sizeof(rcvBuffSize))!=0)
        {display("! ERROR : setsockopt / RCVBUF failed !");}
    
    #ifdef LOG2FILE    
        writeToLog("C[%d] receiving %s on socket %d", connection->index+1, connection->fileName, s);
    #endif

    uint32_t retryNumber = 0;    
	int32_t bytes_received = TRANSFER_BUFFER_SIZE;
    while (bytes_received) {
        
        read_again:
        // use a double sized buffer for recv operation
        bytes_received = network_readChunk(s, connection->transferBuffer, TRANSFER_BUFFER_SIZE);
        if (bytes_received == 0) {
            // SUCCESS, no more to write to file
                    
            nbFilesUL++;
            result = 0;
            
        } else if (bytes_received < 0) {

            if (retry(bytes_received)) {
                OSSleepTicks(OSMillisecondsToTicks(NET_RETRY_TIME_STEP_MILLISECS));
                retryNumber++;
                if (retryNumber <= retriesNumber) goto read_again;
            }    
            display("! ERROR : network_read failed = %d afer %d attempts", bytes_received, retriesNumber);
            display("! ERROR : errno = %d (%s)", errno, strerror(errno));
            // result = error, connection will be closed
            result = bytes_received;
            break;
        } else {
            // bytes_received > 0
                        
            // write bytes_received to f
            result = fwrite(connection->transferBuffer, 1, bytes_received, connection->f);
                
            if ((result < 0 && result < bytes_received) || ferror(connection->f) != 0) {
                // error when writing f
                display("! ERROR : failed to write file!");
                display("! ERROR : fwrite = %d and bytes=%d", result, bytes_received);
                display("! ERROR : errno = %d (%s)", errno, strerror(errno));
                result = -100;
                break;
            } else {
                connection->dataTransferOffset += result;
                connection->bytesTransfered = result;                
            }
        }
    }
    
	connection->bytesTransfered = result;
    
    return result;
}

void setFsaFdInNet(int hfd) {
    fsaFd = hfd;
}
