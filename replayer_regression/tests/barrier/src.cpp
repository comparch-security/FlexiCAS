#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
 
int a=0;
 
pthread_mutex_t numlock; 
pthread_barrier_t b; 
 
void* handle(void *data)
{
        pthread_mutex_lock(&numlock);
        a++;
        pthread_mutex_unlock(&numlock);
        pthread_barrier_wait(&b);
        return 0;
}
 
 
int main()
{
        pthread_t t1,t2;
        pthread_barrier_init(&b,NULL,3); 
        pthread_mutex_init(&numlock,NULL);
        pthread_create(&t1,NULL,handle,NULL);
        pthread_create(&t2,NULL,handle,NULL);
        pthread_barrier_wait(&b);
        printf("a=:%d\n",a);
        exit(0);
}
