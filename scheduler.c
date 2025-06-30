#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/shm.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdbool.h>


#define MAX_DOCKS 30
#define MAX_DOCK_CAT 25
#define MAX_SHIP_CAT 25
#define MAX_REG_INC 500
#define MAX_EME_INC 100
#define MAX_CAP_OF_CRANE 30
#define MAX_CARGO_SHIP 200
#define MAX_SOLVERS 8
#define MAX_NEW_SHIP_REQS 100
#define MAX_STR_LEN 100




typedef struct{
    int craneId;
    int capacity;
}Crane;


typedef struct ShipRequest{
    int shipId;
    int timestep;
    int category;                        
    int direction;                        
    int emergency;                        
    int waitingTime;
    int numCargo;                    
    int cargo[MAX_CARGO_SHIP];  
} ShipRequest;


typedef struct{
    int dockId;
    int category;
    int occupied;
    int dockedShipId;
    int dockedDockShipDirection;
    int dockedTimestep;
    int lastCargoTimestep;
    int readyToUndock;
    Crane cranes[MAX_DOCK_CAT];
    int crane_count;
    ShipRequest ship;
}Dock;


typedef struct{
    int shared_mem_key;
    int main_msg_queue_key;
    int num_solvers;
    int solver_msg_queues[MAX_SOLVERS];
    int num_docks;
    Dock docks[MAX_DOCKS];
}SchedulerConfig;


typedef struct MessageStruct {
    long mtype;  
    int timestep;
    int shipId;
    int direction;
    int dockId;
    int cargoId;
    int isFinished;
    union {
        int numShipRequests;
        int craneId;
    };
} MessageStruct;

typedef struct{
    int cargoId;
    int processed;
    int weight;
}CargoItem;
typedef struct MainSharedMemory{
    char authStrings[MAX_DOCKS][MAX_STR_LEN];
    ShipRequest newShipRequests[MAX_NEW_SHIP_REQS];
} MainSharedMemory;


typedef struct SolverRequest{
    long mtype;
    int dockId;
    char authStringGuess[MAX_STR_LEN];
}SolverRequest;


typedef struct SolverResponse{
    long mtype;
    int guessIsCorrect;
}SolverResponse;


typedef struct{
    ShipRequest data[MAX_NEW_SHIP_REQS*10];
    int front;
    int rear;
}Queue;


typedef struct{
    int solver_queue_key;
    SolverRequest setupMsg;
}SolverThreadArgs;


typedef struct {
    int thread_id;
    int num_solvers;
    int total;
    int length;
    int solver_q;
    int dockId;
    char* result;
    atomic_bool* found;
    pthread_mutex_t* result_lock;
} ThreadArgs;

typedef struct{
    int main_msg_queue;
    Dock* dock;
    int timestep;
} DockThreadArgs;

char valid_chars[] = {'5','6','7','8','9','.'};


bool is_valid_string(char* str, int length){
    return !(str[0] == '.' || str[length-1] == '.');
}


void index_to_string(int index,int length,char* out){
    for(int i= length-1; i >= 0; i--){
        out[i] = valid_chars[index%6];
        index /= 6;
    }
    out[length] = '\0';
}


void* guess_modulo_thread(void* arg) {
    ThreadArgs* args=(ThreadArgs*)arg;
    char guess[MAX_STR_LEN];


    for (int i=0; i<args->total && !*(args->found); i++){
        if (i % args->num_solvers != args->thread_id) continue;


        index_to_string(i, args->length, guess);

        if(!is_valid_string(guess,args->length)) continue;


        SolverRequest req;
        req.mtype = 2;
        strcpy(req.authStringGuess, guess);


        if (msgsnd(args->solver_q, &req, sizeof(SolverRequest) - sizeof(long), 0) == -1){
            perror("msgsnd");
            continue;
        }


        SolverResponse resp;
        if (msgrcv(args->solver_q, &resp, sizeof(SolverResponse) - sizeof(long), 3, 0) == -1){
            perror("msgrcv");
            continue;
        }


        if (resp.guessIsCorrect){
            pthread_mutex_lock(args->result_lock);
            if (!*(args->found)) {
                strcpy(args->result, guess);
                *(args->found) = true;
            }
            pthread_mutex_unlock(args->result_lock);
            break;
        }
    }


    return NULL;
}


void InitQueue(Queue* q){
    q->front = 0;
    q->rear = 0;
}


void enqueue(Queue* q, ShipRequest ship){
    if ((q->rear + 1) % 1000 == q->front) {
        printf("Queue is full. Cannot enqueue.\n");
        return;
    }

    q->data[q->rear] = ship;
    q->rear = (q->rear + 1) % 1000;
}

void enQueueRegularShips(Queue* q,ShipRequest ship){
    int priority = ship.timestep + ship.waitingTime;
    if ((q->rear + 1) % 1000 == q->front) {
        printf("Queue is full. Cannot enqueue.\n");
        return;
    }


    int size = (q->rear-q->front+1000)%1000;
    int insertPos = q->rear;
    int i = size;
    while (i > 0){
        int prevIndex=(q->front + i-1)%MAX_NEW_SHIP_REQS;
        int currIndex= (q->front +i)%MAX_NEW_SHIP_REQS;

        int currPriority= q->data[prevIndex].timestep + q->data[prevIndex].waitingTime;

        if (priority >= currPriority) break;

        q->data[currIndex] = q->data[prevIndex];
        i--;
    }

    int insertIndex = (q->front + i)%1000;
    q->data[insertIndex] = ship;
    q->rear = (q->rear + 1)%1000;
}


ShipRequest dequeue(Queue* q) {
    ShipRequest empty = {0};
    if (q->front == q->rear) {
        printf("Queue is empty. Cannot dequeue.\n");
        return empty;
    }


    ShipRequest ship = q->data[q->front];
    q->front = (q->front+1)%1000;
    return ship;
}


int isQueueEmpty(Queue* q){
    return q->front == q->rear;
}


int getQueueSize(Queue* q){
    if(q->rear >= q->front){
        return q->rear - q->front;
    }
    else{
        return (1000 - q->front + q->rear);
    }
}


Queue Emergency_queue;
Queue Regular_Queue;
Queue OutGoing_queue;

pthread_mutex_t dock_mutex[MAX_DOCKS];
pthread_mutex_t shared_mem_mutex;

void read_input(const char *fileName,SchedulerConfig *config){

    for (int i = 0; i < MAX_DOCKS; i++) {
        config->docks[i].dockId = i;
        config->docks[i].category = 0;
        config->docks[i].crane_count = 0;
        config->docks[i].occupied = 0;
        config->docks[i].dockedShipId = -1;
        config->docks[i].dockedTimestep = -1;
        config->docks[i].readyToUndock = 0;
        config->docks[i].lastCargoTimestep = -1;
        for (int j = 0; j < MAX_DOCK_CAT; j++) {
            config->docks[i].cranes[j].craneId = j;
            config->docks[i].cranes[j].capacity = 0;
        }
    }

    FILE *file = fopen(fileName, "r");
    if(!file){
        perror("Error opening file");
        exit(EXIT_FAILURE);
    }
    fscanf(file, "%d", &config->shared_mem_key);
    fscanf(file,"%d",&config->main_msg_queue_key);
    fscanf(file,"%d",&config->num_solvers);


    if(config->num_solvers < 2 || config->num_solvers > MAX_SOLVERS){
        fprintf(stderr, "error: Number of solvers bw 2 and 8 \n");
        exit(EXIT_FAILURE);
    }


    for(int i=0; i < config->num_solvers; i++){
        fscanf(file,"%d",&config->solver_msg_queues[i]);
    }


    fscanf(file,"%d",&config->num_docks);
    if(config->num_docks < 1 || config->num_docks > MAX_DOCKS){
        fprintf(stderr,"Invalid docks\n");
        fclose(file);
        exit(EXIT_FAILURE);
    }




    for (int i = 0; i < config->num_docks; i++) {
        config->docks[i].dockId = i;
        fscanf(file, "%d", &config->docks[i].category);
        config->docks[i].crane_count = config->docks[i].category;
       
        if (config->docks[i].crane_count > MAX_DOCK_CAT) {
            fprintf(stderr, "Error: Too many cranes at dock %d (max %d allowed).\n", i, MAX_DOCK_CAT);
            fclose(file);
            exit(EXIT_FAILURE);
        }


        for (int j = 0; j < config->docks[i].crane_count; j++) {
            config->docks[i].cranes[j].craneId= j;
            fscanf(file, "%d", &config->docks[i].cranes[j].capacity);
        }
    }


    fclose(file);
}


int Docking(int main_message_queue,SchedulerConfig* config, MainSharedMemory* shm, ShipRequest* ship,int timestep,int reg){
    if(reg == 0){
        if(timestep > ship->timestep + ship->waitingTime){
            //printf("This ship %d, was waiting here forever, will return\n",ship->shipId);
            return 1;
        }
    }

    Dock* bestDock = NULL;

    for(int i=0; i < config->num_docks; i++){
        Dock* dock = &config->docks[i];
        if(!dock->occupied && dock->category >= ship->category){
            if(bestDock == NULL || dock->category < bestDock->category){
                bestDock = dock;
            }
        }
    }

    if(bestDock != NULL){
        bestDock->occupied = 1;
        bestDock->dockedShipId = ship->shipId;
        bestDock->dockedTimestep = timestep;
        bestDock->dockedDockShipDirection = ship->direction;
        bestDock->lastCargoTimestep = -1;
        bestDock->readyToUndock = 0;
        bestDock->ship = *(ship);

        MessageStruct dock_msg;
        dock_msg.mtype = 2;
        dock_msg.dockId = bestDock->dockId;
        dock_msg.shipId = ship->shipId;
        dock_msg.direction = ship->direction;

        if (msgsnd(main_message_queue, &dock_msg, sizeof(dock_msg) - sizeof(long), 0) == -1) {
            perror("Error Docking\n");
            exit(0);
        }

        return 1;
    }
    return 0;
}


void loadUnload(int main_msg_queue,SchedulerConfig* config, MainSharedMemory* shm,int timestep,int num_requests){
    
    for(int d = 0; d < config->num_docks; d++){
        Dock* dock = &config->docks[d];
        
        if(!dock->occupied || dock->dockedTimestep == 0 ||timestep == dock->dockedTimestep){
            continue;
        }
    
        ShipRequest* dockedShip = &dock->ship;
        int crane_used[MAX_DOCK_CAT] = {0};
        //printf("%d dockedship NumCargo\n",dockedShip->numCargo);
        for(int k=0; k < dockedShip->numCargo; k++){
            if(dockedShip->cargo[k]==-24) continue;

            int cargoSize = dockedShip->cargo[k];
            int bestCraneIdx = -1;
            int minCap = 10000;

            for(int c = 0; c < dock->crane_count; c++){
                if(crane_used[c]) continue;

                //printf("Assigning a crane\n");

                int craneCap = dock->cranes[c].capacity;

                if(craneCap >= cargoSize && craneCap < minCap){
                    minCap = craneCap;
                    bestCraneIdx = c;
                }
            }
            if(bestCraneIdx != -1){
                MessageStruct cargoMsg;
                cargoMsg.mtype = 4;
                cargoMsg.dockId = dock->dockId;
                cargoMsg.shipId = dockedShip->shipId;
                cargoMsg.direction = dockedShip->direction;
                cargoMsg.cargoId = k;
                cargoMsg.craneId = dock->cranes[bestCraneIdx].craneId;
                if (msgsnd(main_msg_queue, &cargoMsg, sizeof(cargoMsg) - sizeof(long), 0) == -1) {
                    perror("msgsnd for cargo failed");
                    exit(EXIT_FAILURE);
                }
                dockedShip->cargo[k]  = -24;
                crane_used[bestCraneIdx] = 1;
                dock->lastCargoTimestep = timestep;
            }
            
        }

        
        int allDone = 1;
        for (int i = 0; i < dockedShip->numCargo; i++) {
            if (dockedShip->cargo[i] != -24) {
                allDone = 0;
                break;
            }
        }

        if (allDone) {
            dock->readyToUndock = 1;
        }
    }

}

int generate_auth_string(int num_solvers, char* str, int length, SchedulerConfig* config, int dockId) {
    int total = 1;
    for (int i = 0; i < length; i++) total *= 6;


    pthread_t threads[num_solvers];
    ThreadArgs args[num_solvers];
    atomic_bool found = false;
    pthread_mutex_t result_lock = PTHREAD_MUTEX_INITIALIZER;


    for (int i = 0; i < num_solvers; i++) {
        args[i].thread_id = i;
        args[i].num_solvers = num_solvers;
        args[i].total = total;
        args[i].length = length;
        args[i].dockId = dockId;
        args[i].result = str;
        args[i].found = &found;
        args[i].result_lock = &result_lock;


        args[i].solver_q = msgget(config->solver_msg_queues[i], IPC_CREAT | 0666);
        if (args[i].solver_q == -1) {
            perror("msgget");
            return 0;
        }


        pthread_create(&threads[i], NULL, guess_modulo_thread, &args[i]);
    }


    for (int i = 0; i < num_solvers; i++) {
        pthread_join(threads[i], NULL);
    }


    return found;
}


void* send_to_solver(void* arg) {
    SolverThreadArgs* args = (SolverThreadArgs*) arg;
    int qid = msgget(args->solver_queue_key, IPC_CREAT | 0666);
    if(msgsnd(qid, &args->setupMsg, sizeof(SolverRequest) - sizeof(long), 0) == -1){
        perror("Error sending msg for solver");
        pthread_exit((void*)1);
    }
    pthread_exit(NULL);
}


void unDocking(int main_msg_queue,int solver_msg_queue[],Dock* dock, MainSharedMemory* shared_memory,SchedulerConfig* config){
    if(!dock->occupied){
        printf("No ship at dock %d to undock.\n",dock->dockId);
        return;
    }


    int dockingTime = dock->dockedTimestep;
    int lastCargoTime = dock->lastCargoTimestep;
    int stringLength = lastCargoTime-dockingTime;


    SolverRequest setupMsg;
    setupMsg.mtype = 1;
    setupMsg.dockId = dock->dockId;
    pthread_t SolverThreads[config->num_solvers];
    SolverThreadArgs thread_args[config->num_solvers];

    for(int i=0; i < config->num_solvers; i++){
        thread_args[i].solver_queue_key = solver_msg_queue[i];
        thread_args[i].setupMsg = setupMsg;
   
        if(pthread_create(&SolverThreads[i], NULL, send_to_solver, &thread_args[i]) != 0){
            perror("Failed to create thread");
            exit(EXIT_FAILURE);
        }
    }


    for (int i = 0; i < config->num_solvers; i++) {
        pthread_join(SolverThreads[i], NULL);
    }


    char authString[MAX_STR_LEN];


    int solver_q = msgget(solver_msg_queue[0], IPC_CREAT | 0666);
    if(!generate_auth_string(config->num_solvers,authString,stringLength,config,dock->dockId)){
        printf("Failed to find validation for dock %d\n",dock->dockId);
        return;
    }


    strncpy(shared_memory->authStrings[dock->dockId],authString,MAX_STR_LEN);
  

    MessageStruct undockMsg;
    undockMsg.mtype = 3;
    undockMsg.dockId = dock->dockId;
    undockMsg.shipId = dock->dockedShipId;
    undockMsg.direction = dock->dockedDockShipDirection;


    //printf("Correct Request: direction: %d",correctRequest->direction);




    if(msgsnd(main_msg_queue,&undockMsg,sizeof(undockMsg)-sizeof(long),0) == -1){
       perror("Error in sending undocking msg\n");
       exit(EXIT_FAILURE);
    }
    dock->occupied = 0;
    //printf("Ship %d undocked from dock %d\n",dock->dockedShipId,dock->dockId);
}


void setup_ipc(SchedulerConfig* config, int* main_msg_queue,int* shm_id, MainSharedMemory **shared_memory){
    *main_msg_queue = msgget(config->main_msg_queue_key,IPC_CREAT | 0666);
    if(*main_msg_queue == -1){
        perror("Error creating main message queue\n");
        exit(EXIT_FAILURE);
    }
     
    *shm_id = shmget(config->shared_mem_key,sizeof(MainSharedMemory),IPC_CREAT | 0666);
    if(*shm_id == -1){
        perror("Error accessing shared memory\n");
        exit(EXIT_FAILURE);
    }
    *shared_memory = (MainSharedMemory *)shmat(*shm_id, NULL, 0);
    if(*shared_memory == (void *)-1){
        perror("Error attaching shared memory\n");
        exit(EXIT_FAILURE);
    }
    printf("IPC SETUP COMPLETE\n");
}

int compare_numCargo(const void *a, const void *b) {
    ShipRequest *shipA = (ShipRequest *)a;
    ShipRequest *shipB = (ShipRequest *)b;
    return shipA->numCargo - shipB->numCargo;
}


void sort_queue_by_numCargo(Queue *q) {
    int size = MAX_NEW_SHIP_REQS * 10;
    int n = (q->rear - q->front + size) % size;
    if (n <= 1) return;

    ShipRequest temp[n];
    for (int i = 0; i < n; ++i) {
        temp[i] = q->data[(q->front + i) % size];
    }
    qsort(temp, n, sizeof(ShipRequest), compare_numCargo);
    for (int i = 0; i < n; ++i) {
        q->data[(q->front + i) % size] = temp[i];
    }
}

void poll_requests(SchedulerConfig* config,int main_msg_queue,MainSharedMemory* shared_memory){
    while(1){
        MessageStruct rcvMsg;
        if(msgrcv(main_msg_queue,&rcvMsg,sizeof(rcvMsg)-sizeof(long),1,0) == -1){
            if(errno == ENOMSG){
                continue;
            }
            perror("Error in msgRcv\n");
            exit(EXIT_FAILURE);
        }
        if(rcvMsg.isFinished == 1){
            printf("Testcase has concluded\n");
            break;
        }
        int current_timestamp = rcvMsg.timestep;
        int num_requests = rcvMsg.numShipRequests;


        printf("Current Timestep: %d \n",current_timestamp);
        pthread_mutex_lock(&shared_mem_mutex);
        for(int i=0; i < num_requests; i++){
            ShipRequest* request = &shared_memory->newShipRequests[i];
           // printf("Direction %d, emergency %d\n",request->direction,request->emergency);
            if(request->direction == 1 && request->emergency == 1){
                enqueue(&Emergency_queue,*request);
            }
            else if(request->direction == -1){
                enqueue(&OutGoing_queue,*request);
            }
            else{
                enQueueRegularShips(&Regular_Queue,*request);
            }
        }

        // sort_queue_by_numCargo(&Emergency_queue);
        // sort_queue_by_numCargo(&Regular_Queue);
        // sort_queue_by_numCargo(&OutGoing_queue);


        int size_em = getQueueSize(&Emergency_queue);
        int size_out = getQueueSize(&OutGoing_queue);
        int size_reg = getQueueSize(&Regular_Queue);


        for(int i=0; i < size_em; i++){
            ShipRequest ship = dequeue(&Emergency_queue);
            //printf("Ship->direction %d\n",ship.direction);
            int docked = Docking(main_msg_queue,config,shared_memory,&ship,current_timestamp,1);
            if(!docked){
                enqueue(&Emergency_queue,ship);
            }
        }

        for(int i=0; i < size_reg; i++){
            ShipRequest ship = dequeue(&Regular_Queue);
            int docked = Docking(main_msg_queue,config,shared_memory,&ship,current_timestamp,0);
            if(docked == 2) continue;
            else if(docked == 0){
                enQueueRegularShips(&Regular_Queue,ship);
            }
        }

        for(int i=0; i < size_out; i++){
            ShipRequest ship = dequeue(&OutGoing_queue);
            //printf("Ship->direction %d\n",ship.direction);
            int docked = Docking(main_msg_queue,config,shared_memory,&ship,current_timestamp,1);
            if(!docked){
                enqueue(&OutGoing_queue,ship);
            }
        }
        pthread_mutex_unlock(&shared_mem_mutex);
  
        loadUnload(main_msg_queue,config,shared_memory,current_timestamp,num_requests);


        for(int i=0; i < config->num_docks; i++){
            Dock* dock= &config->docks[i];
            pthread_mutex_lock(&dock_mutex[i]);
           // printf("%d dock occupied, dock last CargoTimestemp: %d, and dock ready to undock = %d\n",dock->occupied,dock->lastCargoTimestep,dock->readyToUndock);
            if(dock->occupied && dock->lastCargoTimestep != -1 && dock->lastCargoTimestep < current_timestamp && dock->readyToUndock == 1){
                unDocking(main_msg_queue,config->solver_msg_queues,dock,shared_memory,config);
            }
            pthread_mutex_unlock(&dock_mutex[i]);
        }



        MessageStruct msge;
        msge.mtype = 5;


        if(msgsnd(main_msg_queue,&msge,sizeof(msge)-sizeof(long),0) == -1){
            perror("Error Updating timestamp\n");
            exit(EXIT_FAILURE);
        }
        printf("TimeStamp update request sent\n");
        usleep(1);
    }
}

int main(int argc,char* argv[]){    
    if(argc != 2){
        perror("Error in command Line args\n");
        exit(0);
    }
    SchedulerConfig sched;
    char fileName[256];
    snprintf(fileName,sizeof(fileName),"testcase%s/input.txt",argv[1]);
   
    read_input(fileName,&sched);


    int main_msg_queue;
    int shm_id;
    InitQueue(&Emergency_queue);
    InitQueue(&Regular_Queue);
    InitQueue(&OutGoing_queue);
    MainSharedMemory *shared_memory;


    setup_ipc(&sched, &main_msg_queue, &shm_id, &shared_memory);
    for(int i=0; i < sched.num_docks; i++){
        pthread_mutex_init(&dock_mutex[i],NULL);
    }
    pthread_mutex_init(&shared_mem_mutex,NULL);
    //InitShipRequestQueue(&queue);
   
    poll_requests(&sched,main_msg_queue,shared_memory);


    for(int i = 0; i < sched.num_docks; i++) {
        pthread_mutex_destroy(&dock_mutex[i]);
    }
    pthread_mutex_destroy(&shared_mem_mutex);


    return 0;
}