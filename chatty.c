/*
 * membox Progetto del corso di LSO 2017/2018
 *
 * Dipartimento di Informatica Università di Pisa
 * Docenti: Prencipe, Torquati
 *
 */
/**
 * @file chatty.c
 * @brief File principale del server chatterbox
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include "stats.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/un.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <string.h>
#include <pthread.h>
#include <getopt.h>
#include "./lib/GestioneQueue/queue.h"
#include "./lib/GestioneListe/linklist.h"
#include "./lib/GestioneHashTable/icl_hash.h"
#include "./utility.h"
#include "./connections.h"
#include "./parser.h"
#include "./config.h"


/* Definizione funzioni */
int createSocket();
int aggiorna(fd_set * set, int max);
int isPipe(int fd);
static void * listener(void * arg);
static void * pool(void * arg);
static void * segnali(void * arg);
void initHashLock();

/* Variabili globali */
static Queue_t * richieste;
static config configurazione;
static int nSocketUtenti = 0;
static pthread_mutex_t nSocketUtenti_m = PTHREAD_MUTEX_INITIALIZER;
static linked_list_t * utentiConnessi;
static pthread_mutex_t hashLock[HASHSIZE / HASHGROUPSIZE];
/*
  utenti connessi a cosa serve?
  ogni volta che un utente mi fa una CONNECT_OP lo devo aggiungere alla lista.
  quando farà una richiesta in cui serve essere connessi, dovrò controllare se è presente
  quando un altro utente invia un messaggio a un client devo verificare se è connesso, semmai glielo posso mandare.
    In ogni caso devo aggiungere il messaggio nella history del ricevente
  quando un utente registrato  si disconnette dovrò toglierlo dalla lista
  QUINDI:
      - lettura e scrittura in mutua esclusione
      - serve una cond? NO
  OSS: utentiConnessi non sarà mai più grande delle socket attive (MaxConnections)
*/
static icl_hash_t * utentiRegistrati;

/* Funzione di hash */
static inline unsigned int fnv_hash_function( void *key, int len ) {
    unsigned char *p = (unsigned char*)key;
    unsigned int h = 2166136261u;
    int i;
    for ( i = 0; i < len; i++ )
        h = ( h * 16777619 ) ^ p[i];
    return h;
}
static inline unsigned int ulong_hash_function( void *key ) {
    int len = strlen((char *)(key));
    unsigned int hashval = fnv_hash_function( key, len );
    return hashval;
}
static inline int ulong_key_compare( void *key1, void *key2  ) {
    return !strcmp((char *) key1, (char *) key2);
}

struct statistics chattyStats = { 0,0,0,0,0,0,0 };

static void usage(const char *progname) {
    fprintf(stderr, "Il server va lanciato con il seguente comando:\n");
    fprintf(stderr, "  %s -f conffile\n", progname);
}


int main(int argc, char *argv[]) {
    /* Prendo il file di configurazione */
    int optc;
    const char optstring[] = "f:";
    char * pathFileConf = NULL;// TODO va deallocata?
    while ((optc = getopt(argc, argv, optstring)) != -1) {
        switch (optc) {
            case 'f':
                pathFileConf = optarg;
            break;
            default:
                usage(argv[0]);
                return -1;
        }
    }
    if(pathFileConf == NULL){
        usage(argv[0]);
        return -1;
    }
    int i;

    /* Inizializzazione */
    initParseCheck(pathFileConf, &configurazione);//TODO controllare errori
    richieste = initQueue();
    utentiRegistrati = icl_hash_create(HASHSIZE, ulong_hash_function, ulong_key_compare);
    utentiConnessi = list_create();
    initHashLock();

    /* Creazione pipe */
    int ** pfds = malloc(sizeof(int *) * configurazione.ThreadsInPool);
    for(i=0; i<configurazione.ThreadsInPool; i++){
        pfds[i] = malloc(sizeof(int) * 2);
        if(pipe(pfds[i]) == -1){ //TODO gestione errore
            perror("Errore pipe\n");
            exit (-1);
        }
    }

    /* Gestione segnali */

    /* Avvio listener */
    pthread_t listener_id;
    pthread_create(&listener_id, NULL, &listener, pfds);

    /* Avvio pool */
    pthread_t * pool_id = malloc(sizeof(pthread_t) * configurazione.ThreadsInPool); // TODO gestione Errore
    for(i=0; i<configurazione.ThreadsInPool; i++){
        pthread_create(&(pool_id[i]), NULL, &pool, &(pfds[i][1]));
    }

    /* Join */
    int * ret_listener;
    pthread_join(listener_id, (void **)&ret_listener);
    int * ret_pool;
    for(i=0; i<configurazione.ThreadsInPool; i++){
        pthread_join(pool_id[i], (void **)&ret_pool);
    }
    free(pathFileConf);
    return 0;
}



void initHashLock(){
    int i=0;
    for(i=0; i<(HASHSIZE / HASHGROUPSIZE); i++){
        pthread_mutex_init(&(hashLock[i]), NULL);
    }
}

static void * pool(void * arg){
    int * fd;
    int pipe = *((int *)arg);
    message_t msg;
    while(1){
        fd = (int *)pop(richieste);
        int r = readMsg(*fd, &msg);
        if(r < 0){ //TODO controllare errori. Non necessita di lock fd dato che un utente può fare una richiesta alla volta che viene presa in gestione da un solo thread del pool
            free(fd);
            perror("Errore readMsg");
            exit(-1);
        }else if(r == 0){// vuol dire che il client ha finito di comunicare, allora devo chiudere la connessione e decrementare nSocketUtenti
            close(*fd); //TODO controllare errore
            pthread_mutex_lock(&nSocketUtenti_m);
            nSocketUtenti --;
            pthread_mutex_unlock(&nSocketUtenti_m);
        }else{
            /*switch(msg.hdr.op){
                case REGISTER_OP:
                    register_op(msg, utentiRegistrati, hashLock, utentiConnessi); //TODO controllare errori
                    break;
                case CONNECT_OP:
                    connect_op(msg, utentiRegistrati, hashLock, utentiConnessi);
                    break;
                case POSTTXT_OP:
                    posttxt_op(msg, utentiRegistrati, hashLock, utentiConnessi);
                    break;
                case POSTTEXTALL_OP:
                    posttextall_op(msg, utentiRegistrati, hashLock, utentiConnessi);
                    break;
                case POSTFILE_OP:
                    postfile_op(msg, utentiRegistrati, hashLock, utentiConnessi);
                    break;
                case GETFILE_OP:
                    getfile_op(msg, utentiRegistrati, hashLock, utentiConnessi);
                    break;
                case GETPREVMSGS_OP:
                    getprevmsgs_op(msg, utentiRegistrati, hashLock, utentiConnessi);
                    break;
                case USRLIST_OP:
                    usrlist_op(msg, utentiRegistrati, hashLock, utentiConnessi);
                    break;
                case UNREGISTER_OP:
                    unregister_op(msg, utentiRegistrati, hashLock, utentiConnessi);
                    break;
                case DISCONNECT_OP:
                    disconnect_op(msg, utentiRegistrati, hashLock, utentiConnessi);
                    break;
                default:
                    //TODO gestione errore
            }*/
            printf("RICEVUTA OP: %d\n", msg.hdr.op);
            if(writen(pipe, fd, sizeof(int)) == -1){
                perror("Errore writen");
                exit(-1);//TODO controlla errori
            }
        }
        free(fd);
    }
}

static void * listener(void * arg){
    int fd_skt, fd_num = 0;
    int i;
    fd_set rdset, set;
    if((fd_skt = createSocket()) == -1){
        printf("Errore creazione socket\n");
        exit(-1);
        // TODO gestione errore
    }
    int **pfds = (int **)arg;
    FD_ZERO(&set);
    for(i=0; i<configurazione.ThreadsInPool; i++){
        FD_SET(pfds[i][0], &set);
        if(pfds[i][0] > fd_num) fd_num = pfds[i][0];
    }
    FD_SET(fd_skt,&set);
    if(fd_skt > fd_num) fd_num = fd_skt;
    while (1) {
        rdset = set;
        if (select(fd_num+1,&rdset,NULL,NULL,NULL) == -1) {
            perror("Errore select");
            exit(-1); // TODO gestione errore
        } else {
            int fd;
            for (fd = 0; fd<=fd_num;fd++) {
                if (FD_ISSET(fd, &rdset)) {
                    if (fd == fd_skt) { /* sock connect pronto */
                        int fd_c = accept(fd_skt, NULL, 0);
                        //TODO lockare nSocketUtenti prima di utilizzarla
                        pthread_mutex_lock(&nSocketUtenti_m);
                        if(nSocketUtenti >= configurazione.MaxConnections){
                            pthread_mutex_unlock(&nSocketUtenti_m);
                            message_t m;
                            setHeader(&(m.hdr), OP_FAIL, "");
                            setData(&(m.data), "", "Troppi utenti collegati", 23);
                            sendRequest(fd_c, &m);
                            close(fd_c);
                        } else {
                            nSocketUtenti ++;
                            pthread_mutex_unlock(&nSocketUtenti_m);
                            FD_SET(fd_c, &set);
                            if (fd_c > fd_num) fd_num = fd_c;
                        }
                    } else {
                        int p;
                        if((p = isPipe(fd)) == 1){ // un thread del pool mi sta comunicando che un fd è di nuovo disponibile
                            int fd_c;
                            if(readn(fd, &fd_c, sizeof(int)) == -1){ //TODO gestione errore
                                perror("Errore read su pipe");
                                exit(-1);
                            }
                            FD_SET(fd_c, &set);
                            if (fd_c > fd_num) fd_num = fd_c;
                        }else if(p == 0){ // un client mi sta mandando una richiesta
                            int * toPush = malloc(sizeof(int));
                            *toPush = fd;
                            if(push(richieste, toPush) == -1){ // TODO gestione errore
                                perror("Errore push");
                                exit(-1);
                            }
                            FD_CLR(fd, &set);
                            fd_num = aggiorna(&set, fd_num);
                        }else{
                            // TODO gestione errore
                            perror("Errore isPipe");
                            exit(-1);
                        }
                    }
                }
            }
        }
    }
}

static void * segnali(void * arg){}
/**
* @return: -1 se ci sono stati errori
*           1 se fd è una pipe
*           0 altrimenti
*/
int isPipe(int fd){
    struct stat file;
    ec_meno1_return(fstat(fd, &file), "Errore stat file");
    return S_ISFIFO(file.st_mode);
}

int aggiorna(fd_set * set, int max){
    int fd, r = 0;
    for(fd=0; fd<=max; fd++){
        if(FD_ISSET(fd, set)){
            r = fd;
        }
    }
    return r;
}
