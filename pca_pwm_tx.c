/*
 * Copyright (c) 2020, NXP. All rights reserved.
 * Distributed under The MIT License.
 * Author: Abraham Rodriguez <abraham.rodriguez@nxp.com>
 * 		   & Landon Haugh <landon.haugh@nxp.com>
 *
 * Description:
 *
 * Transmits an UAVCAN Heartbeat message over a virtual SocketCAN bus.
 *
 */

// UAVCAN specific includes
#include <uorb/pca_pwm.h>
#include <libcanard/canard.h>
#include <o1heap/o1heap.h>
#include <socketcan/socketcan.h>

// Linux specific includes
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <pthread.h>

// Defines
#define O1HEAP_MEM_SIZE 4096
#define NODE_ID 96
#define UPTIME_SEC_MAX 31
#define TX_PROC_SLEEP_TIME 5000

// Function prototypes
void *process_canard_TX_stack(void* arg);
static void* memAllocate(CanardInstance* const ins, const size_t amount);
static void memFree(CanardInstance* const ins, void* const pointer);

// Create an o1heap and Canard instance
O1HeapInstance* my_allocator;
volatile CanardInstance ins;

// Transfer ID
static uint8_t my_message_transfer_id = 0;

// Buffer for serialization of a heartbeat message
//size_t hbeat_ser_buf_size = uavcan_node_Heartbeat_1_0_EXTENT_BYTES_;
//uint8_t hbeat_ser_buf[uavcan_node_Heartbeat_1_0_EXTENT_BYTES_];

const struct pca_pwm_s pca_pwm;
struct pca_pwm_s *pca_pwm_ptr = &pca_pwm;
size_t pca_pwm_size = sizeof(struct pca_pwm_s);

// vcan0 socket descriptor
int s;

int main(void)
{
	// Allocate 4KB of memory for o1heap.
    void *mem_space = malloc(O1HEAP_MEM_SIZE);
    
    // Initialize o1heap
    my_allocator = o1heapInit(mem_space, (size_t)O1HEAP_MEM_SIZE, NULL, NULL);
    
    int sock_ret = open_can_socket(&s);

    // Make sure our socket opens successfully.
    if(sock_ret < 0)
    {
        perror("Socket open");
        return -1;
    }
    
    // Initialize canard as CANFD and node no. 96
    ins = canardInit(&memAllocate, &memFree);
    ins.mtu_bytes = CANARD_MTU_CAN_FD;
    ins.node_id = NODE_ID;

    // Initialize thread for processing TX queue
    pthread_t thread_id;
    int exit_thread = 0;
    pthread_create(&thread_id, NULL, &process_canard_TX_stack, (void*)&exit_thread);
    
    // Main control loop. Run until break condition is found.
    for(;;)
    {
        // Sleep for 1 second so our uptime increments once every second.
        sleep(1);

        pca_pwm_ptr->timestamp = 0;
        pca_pwm_ptr->pwm_period = 20000U;
        for(int i = 0; i < 16; i++) {
            pca_pwm_ptr->pulse_width[i] = 1500U;
        }
        
        // Print data from Heartbeat message before it's serialized.
        system("clear");
        printf("Preparing to send the following pca_pwm message: \n");
        printf("timestamp: %d\n", pca_pwm.timestamp);
        printf("period: %d\n", pca_pwm.pwm_period);
        printf("width: %d\n", pca_pwm.pulse_width[0]);
        
        // Create a CanardTransfer and give it the required data.
        const CanardTransfer transfer = {
            .timestamp_usec = time(NULL),
            .priority = CanardPriorityNominal,
            .transfer_kind = CanardTransferKindMessage,
            .port_id = 500,
            .remote_node_id = CANARD_NODE_ID_UNSET,
            .transfer_id = my_message_transfer_id,
            .payload_size = pca_pwm_size,
            .payload = &pca_pwm,
        };
          
        // Increment our uptime and transfer ID.
        ++my_message_transfer_id;

        // Push our CanardTransfer to the Libcanard instance's transfer stack.
        int32_t result = canardTxPush((CanardInstance* const)&ins, &transfer);
        
        // Make sure our push onto the stack was successful.
        if(result < 0)
        {
            printf("Pushing onto TX stack failed. Aborting...\n");
            break;
        }
    }

    // Main control loop exited. Wait for our spawned process thread to exit and then free our allocated memory space for o1heap.
    pthread_join(thread_id, NULL);
    free(mem_space);
    return 0;
}

/* Standard memAllocate and memFree from o1heap examples. */
static void* memAllocate(CanardInstance* const ins, const size_t amount)
{
    (void) ins;
    return o1heapAllocate(my_allocator, amount);
}

static void memFree(CanardInstance* const ins, void* const pointer)
{
    (void) ins;
    o1heapFree(my_allocator, pointer);
}

/* Function to process the Libcanard TX stack, package into a SocketCAN frame, and send on the bus. */
void *process_canard_TX_stack(void* arg)
{
    printf("Entered thread.\n");
    for(;;)
    {
        // Run every 5ms to prevent using too much CPU.
        usleep(TX_PROC_SLEEP_TIME);

        // Check to see if main thread has asked this thread to stop.
        int* exit_thread = (int*)arg;

        // Check to make sure there's no frames in the transfer stack, then check to see if this thread should exit.
        // If so, exit the thread.
        if(canardTxPeek((CanardInstance* const)&ins) == NULL && *exit_thread)
        {
            printf("Exiting thread.\n");
            return;
        }

        // Loop through all of the frames in the transfer stack.
        for(const CanardFrame* txf = NULL; (txf = canardTxPeek((CanardInstance* const)&ins)) != NULL;)
        {
            // Make sure we aren't sending a message before the actual time.
            if(true) // txf->timestamp_usec < (unsigned long)time(NULL)
            {
                // Instantiate a SocketCAN CAN frame.
                struct canfd_frame frame;

                // Give payload size.
                // Libcanard states that payload_size != len. Use provided
                // lookup table to correctly populate frame.len
                frame.len = txf->payload_size;
                
                // Give extended can id.
                // Make sure to use CAN_EFF_FLAG or you won't get extended CAN ID.
                frame.can_id = txf->extended_can_id | CAN_EFF_FLAG;
                
                // Copy transfer payload to SocketCAN frame.
                memcpy(&frame.data, txf->payload, txf->payload_size);
                
                // Print RAW can data.
                printf("0x%03X [%d] ",frame.can_id, frame.len);
                for (uint8_t i = 0; i < frame.len; i++)
                        printf("%02X ",frame.data[i]);
                printf(" Sent!\n\n");
                    
                // Send CAN Frame.
                if(send_can_data(&s, &frame) < 0)
                {
                    printf("Fatal error sending CAN data. Exiting thread.\n");
                    return;
                }

                // Pop the sent data off the stack and free its memory.
                canardTxPop((CanardInstance* const)&ins);
                ins.memory_free((CanardInstance* const)&ins, (CanardFrame*)txf);
            }
        }
    }
}



