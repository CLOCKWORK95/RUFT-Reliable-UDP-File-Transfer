/* Client and Server side implementation of RUFT : UPLOAD ENVIRONMENT */
#pragma once
#include "header.h"
#include "Reliable_Data_Transfer.c"
#include "time_toolbox.c"

#define POOLSIZE   10

pthread_mutex_t rcv_window_mutex = PTHREAD_MUTEX_INITIALIZER;


/*  
    SERVER SIDE implementation of RUFT Upload Environment. 
    The following structures are used to receive data from clients uploading files on server's directory.  
*/


struct upload_block {

    int                     sockfd;                             // Socket descriptor for this upload block.

    int                     identifier;                         // Identifier code for this upload block.

    char                    filepath[MAXLINE];                  // Current file pathname ( variable for each upload instance).

    char                    ACK[ACK_SIZE];                      // Buffer used to write and send acknowledgements.

    struct sockaddr_in      *clientaddr;                        // Current client's address.

    int                     addr_len;                           // Current client address' lenght.

    pthread_t               uploader;                           // Uploader thread identifier.

    pthread_t               writer;                             // Writer thread identifier.

    rw_slot                 *rcv_wnd;                           // Pointer to the current block's receiving window.

    char                    uploading;                          // Flag that represents the current state of this upload block (waiting or uploading).

    int                     sem_id;                             // Semaphore array identifier. This is used to handle uploader and writer thread's start.

    struct upload_block     *next;                              // Pointer to the next block of Upload Environment's pool.

};                                                                  struct upload_block         *upload_environment;



/*  
    This is a thread function. The Uploader thread is always coupled with a Writer thread. 
    Each Uploader has the responsibility to operate an upload of a file from a remote client. 
    The Uploader receives data packets and transmits acknowledgements, in order to ensure reliable file transfer.
    Each Uploader communicates with its Writer, making him transcribing all packet's data in a persistent file on server's directory. 
    After the Upload is complete, this thread is blocked and waits to be signaled for a following new upload occurrence.
*/
void    * uploader( void * upload_block ) {

    int                             ret,              num,            counter = 0;

    struct upload_block             *block = (struct upload_block *) upload_block;

    char                            rcv_buffer[MAXLINE];

    struct sembuf oper;

    start: 

    counter = 0;

    oper.sem_flg = 0;
    oper.sem_num = 0;
    oper.sem_op = -1;

    ret = semop( block -> sem_id, &oper, 1 );
    if (ret == -1) {
        printf("\n Error in function semop (uploader). errno %d", errno );
        goto start;
    }

    // Upload File.

    printf("\n PREPEARING TO UPLOAD...\n Getting informations about the uploading file...");            fflush(stdout);

    block -> rcv_wnd = get_rcv_window();

    block -> addr_len = sizeof( struct sockaddr_in );

    /* Receive the file size and the identifier of server worker matched to this download instance. */
    ret = recvfrom( ( block -> sockfd ), (char *) rcv_buffer, MAXLINE,  MSG_WAITALL, 
                    (struct sockaddr *) &( block -> clientaddr ), &( block -> addr_len ) ); 
    if (ret <= 0)       Error_("Error in function : recvfrom (downloader).", 1);

    printf("\n RECEIVED infos : %s", rcv_buffer );                                                      fflush(stdout); 

    /* Initiate the exit-condition's values for the next cycle. */
    char *idtf;
    idtf =                strtok( rcv_buffer, "/" );
    block -> identifier = atoi( idtf );

    printf("\n - WORKER ID : %d", block -> identifier );                                                fflush(stdout); 

    char *filesz;
    filesz =              strtok( NULL, "/" );
    int filesize =        atoi( filesz );

    printf("\n - SIZE : %d", filesize );                                                                fflush(stdout); 

    memset( rcv_buffer, 0, MAXLINE);

    /* Extract a seed from current time and set it for the rand function. */
    srand((unsigned int)time(0));

    do{

        printf("\n  UPLOAD IN PROGRESS... ");                                                           fflush(stdout);

        loss:

        ret = recvfrom( block -> sockfd, (char *) rcv_buffer, MAXLINE,  MSG_WAITALL, 
                        (struct sockaddr *) &( block -> clientaddr ), &( block -> addr_len ) ); 
        if (ret <= 0)       Error_("Error in function : recvfrom (uploader).", 1);

        char    *idtf;                          idtf = strtok( rcv_buffer, "/");
        int     identifier = atoi( idtf );

        num = ( rand() % 100 ) + 1; 
        if( num <= LOSS_PROBABILITY ) goto loss;

        
        if ( identifier == ( block -> identifier ) ) {

            /*  THIS IS A CRITIAL SECTION FOR RECEIVING WINDOWS ACCESS ON WRITING. 
                DOWNLOADER THREAD TAKES A TOKEN FROM MUTEX TO RUN THIS CODE BLOCK. */

            if ( pthread_mutex_lock( &rcv_window_mutex ) == -1 )        
                                    Error_("Error in function : pthread_mutex_lock (uploader).", 1);

            rw_slot     *wnd_tmp = ( block -> rcv_wnd );

            char        *sn = strtok( NULL, "/" );

            int         sequence_number = atoi( sn );

            printf("\033[01;34m");
            printf("\n --> ARRIVED PACKET WITH SEQUENCE NUMBER : %d .", sequence_number);               fflush(stdout);
            printf("\033[0m");

            for (int i = 0; i < WINDOW_SIZE; i++) {

                printf("\n wnd_tmp->sequence_number=%d  sequence_number=%d", 
                                ( wnd_tmp -> sequence_number ), sequence_number);                       fflush(stdout);

                if ( wnd_tmp -> sequence_number == sequence_number ) {

                    /* Send an ACKNOWLEDGMENT to the RUFT Server Side. */

                    sprintf( ( block -> ACK ), "%d/%d/", identifier, sequence_number );

                    printf("\033[01;32m");
                    printf(" SENDING ACK : %s", block -> ACK);
                    printf("\033[0m");

                    ret = sendto( block -> sockfd, (const char *) ( block -> ACK ), MAXLINE, MSG_CONFIRM, 
                                 (const struct sockaddr *) &( block -> clientaddr ), block -> addr_len ); 
                    if (ret <= 0) {
                        printf("\n Error in function : sendto (uploader). errno = %d ", errno );
                        exit(-1);
                    }

                    /* Update rcv_window's slot status.  */
                    wnd_tmp -> status = RECEIVED;

                    wnd_tmp -> packet = malloc( sizeof(char) * MAXLINE );
                    if (wnd_tmp -> packet == NULL)      Error_( "Error in function : malloc (downloader).", 1);
                    if ( sprintf( ( wnd_tmp -> packet ), "%s", ( rcv_buffer + ( strlen(idtf) + strlen(sn) + 2 ) ) ) == -1 )        
                                                                         Error_( "Error in function : sprintf (uploader).", 1);

                    counter += strlen( wnd_tmp -> packet);

                    if ( ( wnd_tmp -> is_first ) == '1' ) {

                        printf( "\n SENDING SIGNAL TO WRITER FOR PACKET %d", sequence_number );      fflush(stdout); 

                        /*  If this is the first slot of the window, then alert the writer about it (SIGUSR2) 
                            so that it could slide the rcv_window on.    */

                        pthread_kill( block -> writer, SIGUSR2 );

                        if ( pthread_mutex_unlock( &rcv_window_mutex ) == -1 )        
                                                        Error_("Error in function : pthread_mutex_unlock (uploader).", 1);

                    }

                    break;
                }

                wnd_tmp = wnd_tmp -> next;

            }

            if ( pthread_mutex_unlock( &rcv_window_mutex ) == -1 )        
                                                        Error_("Error in function : pthread_mutex_unlock (uploader).", 1);


            /* END OF CRITICAL SECTION FOR RECEIVING WINDOW'S ACCESS. */

        }

        printf("\n counter = %d, filesize = %d", counter, filesize);        fflush(stdout);

        memset( rcv_buffer, 0, strlen(rcv_buffer) );

    } while( counter < filesize );

    printf("\033[01;34m");
    printf("\n UPLOAD COMPLETE.");                                          fflush(stdout);
    printf("\033[0m");

    block -> uploading = '0';

    free( block -> rcv_wnd);

    goto start;

}


/*  
    This is a thread function. The Writer thread is always coupled with an Uploader thread.
    Each Writer has the responsibility of writing data coming from the client in a persistent file on server's directory.
    After the Upload is complete, this thread is blocked and waits to be signaled for a following new upload occurrence.
*/
void    * writer( void * upload_block ) {

    int ret;

    struct upload_block             *block = (struct upload_block *) upload_block;

    sigset_t    set;                struct sembuf oper;

    start:

    oper.sem_num = 1;
    oper.sem_op = -1;
    oper.sem_flg = 0;

    ret = semop( block -> sem_id, &oper, 1 );
    if (ret == -1) {
        printf("\n Error in funciton : semop (writer). errno %d", errno);
        goto start;
    }


    /* Temporarily block SIGUSR2 signal occurrences. */
    sigemptyset( &set );
    sigaddset( &set, SIGUSR2);
    sigprocmask( SIG_BLOCK, &set, NULL );

    int            file_descriptor,            counter = 0;

    /* Create the new file in client's directory or truncate an existing one with the same pathname, to start download. */
    file_descriptor = open( ( block -> filepath ), O_RDWR | O_CREAT , 0660 );  
    if (file_descriptor == -1) {
        printf("\n Error in function : open (writer). errno = %d", errno);
        pthread_exit(NULL);
    }

    printf("\n WRITER IS ENTERING THE CYCLE.");                             fflush(stdout);

    do {
        
        if ( block -> uploading == '0') {
            printf("\n All file has been written on server's directory.");  fflush(stdout);
            break;
        }

        printf("\n WRITER IS in THE CYCLE.");                               fflush(stdout);
        /* Be ready to be awaken by SIGUSR2 occurrence. Go on pause. */
        
        sigpending(& set);

        if ( sigismember( &set, SIGUSR2 ) ) {
            signal( SIGUSR2, wake_up );
            sigprocmask( SIG_UNBLOCK, &set, NULL );
            printf("\n SIGUSR2 pending on mask! goto action.");             fflush(stdout);
            goto action;
        }

        signal( SIGUSR2, wake_up );
        sigemptyset( &set );
        sigaddset( &set, SIGUSR2);
        sigprocmask( SIG_UNBLOCK, &set, NULL );

        printf("\n SIGUSR2 UNBLOCKED");                                     fflush(stdout);

        pause();

        action:
        
        printf("\n SIGUSR2 BLOCKED");                                       fflush(stdout);

        /* Temporarily block SIGUSR2 signal occurrences. */
        sigprocmask( SIG_BLOCK, &set, NULL );

        printf( "\n WRITER AWAKED" );                                       fflush(stdout);

        {   
            /*  THIS IS A CRITIAL SECTION FOR RECEIVING WINDOWS ACCESS ON WRITING. 
                WRITER THREAD TAKES A TOKEN FROM MUTEX TO RUN THIS CODE BLOCK. */

            if ( pthread_mutex_lock( &rcv_window_mutex ) == -1 )        Error_("Error in function : pthread_mutex_lock (writer).", 1);
        
            rw_slot      *wnd_tmp = ( block -> rcv_wnd );

            rw_slot      *curr_first   = ( block -> rcv_wnd );

            while( ( curr_first -> status != RECEIVED ) && ( curr_first -> is_first  != '1') ) {
                curr_first = ( curr_first -> next );
            }

            while ( curr_first -> status == RECEIVED ) {

                lseek( file_descriptor, 0, SEEK_END );

                /* Write the packet within the new file in client's directory. */
                ret = write( file_descriptor, ( curr_first -> packet ), strlen( curr_first -> packet  ) );
                if ( ret == -1)         Error_( "Error in function : write (thread writer).", 1);
                
                printf("\033[01;34m");
                printf( "\n PACKET %d CONTENT WRITTEN ON FILE : %s. %d BYTES WRITTEN.", 
                                            ( curr_first -> sequence_number ), ( block -> filepath ), ret );           fflush(stdout);
                printf("\033[0m");


                /* Slide the receiving window on. */

                ( curr_first -> sequence_number ) += WINDOW_SIZE;
                ( curr_first -> status ) = WAITING;
                memset( ( curr_first -> packet ), 0, sizeof( curr_first -> packet ) );
                curr_first -> is_first = '0';

                curr_first -> next -> is_first = '1';
                curr_first = ( curr_first -> next );
                block -> rcv_wnd = curr_first;

                printf("\n WINDOW SLIDED ON");      fflush(stdout);
                
            
            }


            if ( pthread_mutex_unlock( &rcv_window_mutex ) == -1 )        Error_("Error in function : pthread_mutex_unlock (writer).", 1);

            printf("\n WRITER HAS COMPLETED ITS TASK AND GOES TO SLEEP.");                                                 fflush(stdout);

            /* END OF THE CRITICAL SECTION. */

        }



    } while (1);

    block -> uploading = '0';

    goto start;


}


/*
    By this function, RUFT Server initiates the Upload Environment. 
    It consists in a thread pool of size POOLSIZE: once initialized, POOLSIZE 'uploader' and 'writer' threads are launched, 
    and wait for new upload occurrences. A new upload occurrence is signaled by function "start_upload".
*/
int     initialize_upload_environment() {

    int                     ret;

    upload_environment = malloc( sizeof( struct upload_block ) );
    if (upload_environment == NULL ) {
        printf("Error in function : malloc (init_upload_environment). errno %d", errno );
        exit(1);
    }

    struct upload_block     *tmp;

    tmp = upload_environment;

    for ( int i = 0; i < POOLSIZE; i++ ) {

        if ( ( ( tmp -> sockfd ) = socket( AF_INET, SOCK_DGRAM, 0 ) ) < 0 ) {            //creating block's socket file descriptor 
            perror("\n socket creation failed (init new block)."); 
            return -1; 
        } 


        tmp -> sem_id = semget( IPC_PRIVATE, 2, O_CREAT | 0660 );
        if (tmp -> sem_id == -1) {
            printf("\n Error in function : semget (itinialize_upload_environment). errno %d", errno);
            return -1;
        }

        ret = semctl( tmp -> sem_id, 0, SETVAL, 0 );
        if (ret == -1) {
            printf("\n Error in function : semctl (initialize_upload_instance). errno %d", errno);
            return -1;
        }

        ret = semctl( tmp -> sem_id, 1, SETVAL, 0 );
        if (ret == -1) {
            printf("\n Error in function : semctl (initialize_upload_instance). errno %d", errno);
            return -1;
        }


        tmp -> identifier = i;

        tmp -> uploading = '0';

        ret = pthread_create( &( tmp -> uploader ), NULL, uploader, ( void * ) tmp );
        if (ret == -1) {
            printf("Error in function : pthread_create (initialize_upload_environment). errno %d", errno );
            exit(1);
        }

        ret = pthread_create( &( tmp -> writer ), NULL, writer, (void *) tmp );
        if (ret == -1) {
            printf("Error in function : pthread_create (initialize_upload_environment). errno %d", errno );
            exit(1);
        }

        tmp -> next = malloc( sizeof( struct upload_block ) );

        tmp = tmp -> next;

    }

    return 0;

}


/*
    This function is called by RUFT Server's receptionist, to serve a new PUT (upload) request.
    By this function, a waiting slot of upload environment is updated with the new request's informations, and the respective
    couple of waiting uploader&writer threads is signaled to serve the upload request.
*/
int     start_upload( char * filepath, struct sockaddr_in *clientaddress, int len ) {

    int                     ret;                    struct upload_block     *tmp;       
    
    char                    buffer[MAXLINE];

    struct sembuf oper;     oper.sem_flg = 0;       oper.sem_op = 1;

    redo:                   tmp = upload_environment;

    while( ( tmp -> uploading ) != '0') {

        if (tmp -> next == NULL ) goto redo;

        tmp = ( tmp -> next );

    }

    sprintf( tmp -> filepath, "%s", filepath );

    tmp -> clientaddr = clientaddress;

    tmp -> addr_len = len;

    sprintf( buffer, "%d/", ( tmp -> identifier ) );

    printf("\n Buffer content : %s", buffer); fflush(stdout);

    ret = sendto( ( tmp -> sockfd ), (char *) buffer, MAXLINE, MSG_CONFIRM, (struct sockaddr *) ( tmp -> clientaddr ), ( tmp -> addr_len ) ); 
    if (ret <= 0) {
        printf("\n Error in function : sendto (start_upload). errno %d", errno );
        return -1;
    }

    tmp -> uploading = '1'; 


    oper.sem_num = 0;
    ret = semop( tmp -> sem_id, &oper, 1 );
    if (ret == -1) {
        printf("\n Error in function semop (start_upload). errno %d", errno );
        return -1;
    }
    oper.sem_num = 1;
    ret = semop( tmp -> sem_id, &oper, 1 );
    if (ret == -1) {
        printf("\n Error in function semop (start_upload). errno %d", errno );
        return -1;
    }


    return 0;

}