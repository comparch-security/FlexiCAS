#include <pthread.h>

#include <cstdlib>
#include <iostream>


#define MAX_N_WORKER_THREADS 4

typedef struct
{
    int nsteps;
    int* shared_var;
    pthread_spinlock_t* lock;
} ThreadArg;

void* func( void* args )
{
    ThreadArg* my_args = ( ThreadArg* ) args;

    int nsteps = my_args->nsteps;
    int* shared_var = my_args->shared_var;
    pthread_spinlock_t* lock = my_args->lock;

    for ( int i = 0; i < nsteps; ++i ) {
        // acquire the lock
        pthread_spin_lock(lock);

        // increment the shared_var
        (*shared_var)++;

        // release the lock
        pthread_spin_unlock(lock);
    }

    return nullptr;
}

int main( int argc, const char* argv[] )
{
    int n_worker_threads = 0;

    // allocate all threads
    pthread_t* threads = new pthread_t[MAX_N_WORKER_THREADS];
    ThreadArg* t_args = new ThreadArg[MAX_N_WORKER_THREADS];

    // variable shared among all threads
    int shared_var = 0;

    // number of steps each thread increments the shared_var
    int nsteps = 10000;

    // create a shared lock
    pthread_spinlock_t lock;
    pthread_spin_init(&lock, 0);

    int ret;

    // try to spawn as many worker threads as possible
    for ( int tid = 0; tid < MAX_N_WORKER_THREADS; ++tid ) {
        t_args[tid].nsteps = nsteps;
        t_args[tid].shared_var = &shared_var;
        t_args[tid].lock = &lock;

        // spawn thread
        ret = pthread_create( threads + tid, nullptr, func, &t_args[tid] );

        if (ret != 0)
            break;

        n_worker_threads++;
    }

    // sync up all threads
    for ( int tid = 0; tid < n_worker_threads; ++tid ) {
        pthread_join( threads[tid], nullptr );
    }

    // verify
    bool passed = true;
    if ( shared_var != n_worker_threads * nsteps )
        passed = false;

    // clean up
    delete[] threads;
    delete[] t_args;

    if (!passed || n_worker_threads < 1)
        return EXIT_FAILURE;

    return EXIT_SUCCESS;
}
