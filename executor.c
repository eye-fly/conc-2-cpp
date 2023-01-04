#include <assert.h>
// #include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "err.h"
#include "utils.h"

#define MAX_N_TASKS 100 //4096
#define MAX_INPUT_CHARACTERS 511
#define MAX_OUTPUT_CHARACTERS 1022

// typedef struct Executor;

struct Task{
    int task_pid, task_nr;
    pthread_t monitor, out_reader, err_reader;
    pthread_mutex_t out_mutex, err_mutex;
    char out_buffer[2][MAX_OUTPUT_CHARACTERS];
    char err_buffer[2][MAX_OUTPUT_CHARACTERS];
    int last_out, ast_err;
    int out_pipe, err_pipe;

    struct Executor* ex;
};

struct Executor {
    pthread_mutex_t mutex; 
    pthread_cond_t mainT;
    pthread_cond_t input_reader, monitors;
    char input_buffer[MAX_INPUT_CHARACTERS];
    int crr_command;
    int last_command;
    char ** spited_command;

    struct Task tasks[MAX_N_TASKS];
    int tasks_n;

    int exit_task_nr;
    int exit_task_status;
};

void executor_init(struct Executor* ex)
{
    ASSERT_ZERO(pthread_mutex_init(&ex->mutex, NULL));
    ASSERT_ZERO(pthread_cond_init(&ex->mainT, NULL));
    ASSERT_ZERO(pthread_cond_init(&ex->input_reader, NULL));
    ASSERT_ZERO(pthread_cond_init(&ex->monitors, NULL));
    ex->crr_command=0;
    ex->last_command=0;
    ex->tasks_n=0;
    ex->exit_task_nr=-1;
}

void executor_destroy(struct Executor* ex)
{
    ASSERT_ZERO(pthread_mutex_destroy(&ex->mutex));
    ASSERT_ZERO(pthread_cond_destroy(&ex->mainT));
    ASSERT_ZERO(pthread_cond_destroy(&ex->input_reader));
    ASSERT_ZERO(pthread_cond_destroy(&ex->monitors));
}

void* input_reader_main(void* data){
    struct Executor* ex = data;

    FILE *f_in = fdopen(STDIN_FILENO,"r");
    if(f_in == NULL)
        fatal("fdopen filed");

    bool quitting = false;
    while(read_line(ex->input_buffer, MAX_INPUT_CHARACTERS, f_in)){
        ex->crr_command++;

        ASSERT_ZERO(pthread_cond_signal(&ex->mainT));
        if(!strcmp(ex->input_buffer, "quit")){
            quitting = true;
            break;
        }

        ASSERT_ZERO(pthread_mutex_lock(&ex->mutex));
        while(ex->crr_command >  ex->last_command)
            ASSERT_ZERO(pthread_cond_wait(&ex->input_reader, &ex->mutex));
        ASSERT_ZERO(pthread_mutex_unlock(&ex->mutex));
    }
    if(!quitting){
        strcpy(ex->input_buffer,"quit");
        ex->crr_command++;
        ASSERT_ZERO(pthread_cond_signal(&ex->mainT));
    }
    fprintf(stderr, "input reder cloasinf\n");
    return NULL;
}

void* out_reader_main(void* data){
    struct Task* tsk = data;

    FILE *f_in = fdopen(tsk->out_pipe,"r");
    if(f_in == NULL)
        fatal("fdopen filed");
     
    while(read_line(tsk->out_buffer[ (tsk->last_out+1)%2 ], MAX_OUTPUT_CHARACTERS, f_in)){
        ASSERT_ZERO(pthread_mutex_lock(&tsk->out_mutex));
        tsk->last_out = (tsk->last_out+1)%2;
        fprintf(stderr,"on: %d recived: %s\n", tsk->last_out, tsk->out_buffer[ tsk->last_out ]);
        ASSERT_ZERO(pthread_mutex_unlock(&tsk->out_mutex));
    }
    fprintf(stderr, "task: %d reader cloasing\n", tsk->task_pid);
    return NULL;
}

void* task_monitor_main(void* data){
    struct Task* tsk = data;
    int status;
    fprintf(stderr, "monitor start\n");
    waitpid(tsk->task_pid, &status, 0);
    fprintf(stderr, "monitor task finished?\n");

    ASSERT_ZERO(pthread_mutex_lock(&tsk->ex->mutex));
    while(tsk->ex->exit_task_nr != -1)
        ASSERT_ZERO(pthread_cond_wait(&tsk->ex->monitors, &tsk->ex->mutex));
    tsk->ex->exit_task_nr = tsk->task_nr;
    tsk->ex->exit_task_status = status;
    ASSERT_SYS_OK(pthread_cond_signal(&tsk->ex->mainT));
    ASSERT_ZERO(pthread_mutex_unlock(&tsk->ex->mutex));
    fprintf(stderr, "monitor monitor end\n");
    return NULL;
}

void task_starter(struct Executor* ex){
    int my_tsk_nr = ex->tasks_n;
    struct Task* tsk = &ex->tasks[my_tsk_nr];
    tsk->task_nr = my_tsk_nr;

    int pipe_out[2], pipe_err[2];
    ASSERT_SYS_OK(pipe(pipe_out));
    ASSERT_SYS_OK(pipe(pipe_err));

    pid_t pid = fork();
    ASSERT_SYS_OK(pid);
    if (!pid){
        fprintf(stderr,"tasks starting T: %d my_pid: %d \n",my_tsk_nr, getpid() );
        ASSERT_SYS_OK(close(pipe_out[0]));
        ASSERT_SYS_OK(close(pipe_err[0]));

        ASSERT_SYS_OK(dup2(pipe_out[1], STDOUT_FILENO));
        ASSERT_SYS_OK(close(pipe_out[1]));
        ASSERT_SYS_OK(dup2(pipe_err[1], STDERR_FILENO));
        ASSERT_SYS_OK(close(pipe_err[1]));

        ASSERT_SYS_OK(execvp(ex->spited_command[1], &ex->spited_command[1]));
        exit(0);
    }
    ASSERT_SYS_OK(close(pipe_out[1]));
    ASSERT_SYS_OK(close(pipe_err[1]));


    tsk->task_pid = pid;

    tsk->out_pipe = pipe_out[0];
    tsk->err_pipe = pipe_err[0];
    ASSERT_ZERO(pthread_create(&tsk->out_reader, NULL, out_reader_main, (void*)tsk));
    pthread_detach(tsk->out_reader);

    tsk->ex = ex;
    ASSERT_ZERO(pthread_create(&tsk->monitor, NULL, task_monitor_main, (void*)tsk));
    pthread_detach(tsk->monitor);

    printf("Task %d started: pid %d.\n",my_tsk_nr, pid);
    ex->tasks_n++;
}

void read_out(struct Executor* ex, char* c_nr){
    int nr = atoi(c_nr);
    
    pthread_mutex_t * mtx = &ex->tasks[nr].out_mutex;
    ASSERT_ZERO(pthread_mutex_lock(mtx));
    int indx = ex->tasks[nr].last_out;
    printf("Task %d stdout: '%s'.\n", nr, ex->tasks[nr].out_buffer[indx]);
    ASSERT_ZERO(pthread_mutex_unlock(mtx));
}


void finish_task(struct Executor* ex){
    
    if (WIFEXITED(ex->exit_task_status))
        printf("Task %d ended: status %d.\n", ex->exit_task_nr, WEXITSTATUS(ex->exit_task_status));
    else if (WIFSIGNALED(ex->exit_task_status))
        printf("Task %d ended: signalled.\n", ex->exit_task_nr);
    else
        fatal("Unexpected wait() result.");

    ex->tasks[ex->exit_task_nr].task_pid = 0;
    ex->exit_task_nr = -1;
    ASSERT_ZERO(pthread_cond_signal(&ex->monitors));
}

int main(){
    fprintf(stderr,"main my_pid: %d \n", getpid() );

    struct Executor executor;
    executor_init(&executor);
 
    
    pthread_t input_reader_thread;
    int tsk_nr;
    int quiting = -1;
    ASSERT_ZERO(pthread_create(&input_reader_thread, NULL, input_reader_main, (void*)&executor));

    while(1){
        ASSERT_ZERO(pthread_mutex_lock(&executor.mutex));
        while(executor.crr_command <= executor.last_command && executor.exit_task_nr == -1)
            ASSERT_ZERO(pthread_cond_wait(&executor.mainT, &executor.mutex));

        if( executor.exit_task_nr != -1 ){
            finish_task(&executor);
            if(quiting > 0) quiting--;
        } else  if(executor.crr_command > executor.last_command){
            // execute a comand
            executor.spited_command = split_string(executor.input_buffer);

            if (!strcmp(executor.spited_command[0], "run")){
                fprintf(stderr, "run command on task: %s\n", executor.spited_command[1]);
                task_starter(&executor);
            } else if(!strcmp(executor.spited_command[0], "out")){
                read_out(&executor, executor.spited_command[1]);
            } else if(!strcmp(executor.spited_command[0], "kill")){
                tsk_nr = atoi(executor.spited_command[1]);
                if(executor.tasks[tsk_nr].task_pid != -1)
                    kill(executor.tasks[tsk_nr].task_pid, SIGINT);
            } else if(!strcmp(executor.spited_command[0], "sleep")){
                usleep(1000*atoi(executor.spited_command[1]));
            } else if(!strcmp(executor.spited_command[0], "quit")){ 
                quiting = 0;
                for(int i =0; executor.tasks_n >i;i ++){
                    if(executor.tasks[i].task_pid != 0){
                        quiting++;
                        kill(executor.tasks[i].task_pid, SIGINT);
                    }
                }
            } else
                fprintf(stderr, "skiping empty command\n");

            free_split_string(executor.spited_command);
            
            executor.last_command++;
            ASSERT_ZERO(pthread_cond_signal(&executor.input_reader));
        } else{
            exit(1);
        }
        ASSERT_ZERO(pthread_mutex_unlock(&executor.mutex));
        
        if(quiting == 0) break;
    }

    // input_reader_thread
    executor_destroy(&executor);
    ASSERT_ZERO(pthread_join(input_reader_thread,NULL));
    // ASSERT_ZERO(pthread_exit(input_reader_thread));
}