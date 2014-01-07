#ifdef TESTSWAPM
#include <mpi.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#define PIO_NOERR 0
#define min(a,b) \
   ({ __typeof__ (a) _a = (a); \
       __typeof__ (b) _b = (b); \
     _a < _b ? _a : _b; })

#else
#include <pio.h>
#include <pio_internal.h>
#endif

#define MAX_GATHER_BLOCK_SIZE 64


void CheckMPIReturn(const int ierr,const char file[],const int line)
{
  
  if(ierr != MPI_SUCCESS){
    char errstring[MPI_MAX_ERROR_STRING];
    int errstrlen;
    int mpierr = MPI_Error_string( ierr, errstring, &errstrlen);

    fprintf(stderr, "MPI ERROR: %s in file %s at line %d\n",errstring, file, line);
    
  }
}



int pio_fc_gather( void *sendbuf, const int sendcnt, const MPI_Datatype sendtype,
		   void *recvbuf, const int recvcnt, const MPI_Datatype recvtype, const int root, 
		   const MPI_Comm comm, const int flow_cntl)
{
  bool fc_gather;
  int gather_block_size;
  int mytask, nprocs;
  int mtag;
  MPI_Status status;
  int ierr;
  int hs;
  int displs;
  


  if(flow_cntl > 0){
    fc_gather = true;
    gather_block_size = min(flow_cntl,MAX_GATHER_BLOCK_SIZE);
  }else{
    fc_gather = false;
  }

  if(fc_gather){
    CheckMPIReturn(MPI_Comm_rank (comm, &mytask), __FILE__,__LINE__);
    CheckMPIReturn(MPI_Comm_size (comm, &nprocs), __FILE__,__LINE__);

    mtag = 2*nprocs;
    hs = 1;

    if(mytask == root){
      int preposts = min(nprocs-1, gather_block_size);
      int head=0;
      int count=0;
      MPI_Request *rcvid = (MPI_Request *) malloc(gather_block_size * sizeof(MPI_Request));
      
      for(int p=0;p<nprocs;p++){
	if(p != root){
	  if(recvcnt > 0){
	    if(count++ > preposts){
	      int tail = head % preposts +1;
	      CheckMPIReturn(MPI_Wait(rcvid+tail, &status), __FILE__,__LINE__);
	    }
	    head = head % preposts + 1;
	    displs = p*recvcnt;

	    char *ptr = (char *) recvbuf + p;
	    CheckMPIReturn(MPI_Irecv(  ptr, recvcnt, recvtype, p, mtag, comm, rcvid+head), __FILE__,__LINE__);
	    CheckMPIReturn(MPI_Send( &hs, 1, recvtype, p, mtag, comm), __FILE__,__LINE__);
	  }
	}
      }
      displs = mytask*recvcnt;
      
      count = min(count, preposts);
      if(count>0)
	CheckMPIReturn(MPI_Waitall( count, rcvid, MPI_STATUSES_IGNORE), __FILE__,__LINE__);
      free(rcvid);
    }else{
      if(sendcnt > 0){
	CheckMPIReturn(MPI_Recv( &hs, 1, sendtype, root, mtag, comm, &status), __FILE__,__LINE__);
	CheckMPIReturn(MPI_Rsend( sendbuf, sendcnt, sendtype, root, mtag, comm), __FILE__,__LINE__);
      }
    }
  }else{
    CheckMPIReturn(MPI_Gather ( sendbuf, sendcnt, sendtype, recvbuf, recvcnt, recvtype, root, comm), __FILE__,__LINE__);
  }
  return PIO_NOERR;
}
  
int ceil2(const int i)
{
  int p=1;
  while(p<i){
    p*=2;
  }
  return(p);
}

int pair(const int np, const int p, const int k)
{
  int q = (p+1) ^ k ;
  int pair = (q > np-1)? -1: q;
  return pair;
}

int pio_swapm(const int nprocs, const int mytask, void *sndbuf, const int sbuf_size, const int sndlths[],
	      const int sdispls[], const MPI_Datatype stypes[], void *rcvbuf, const int rbuf_size, const int rcvlths[], 
	      const int rdispls[], const MPI_Datatype rtypes[], const MPI_Comm comm, const bool handshake, const bool isend, 
	      const int max_requests)
{
  int tag;
  int offset_t;
  int ierr;
  MPI_Request rcvids[nprocs];
  MPI_Request sndids[nprocs];
  MPI_Request hs_rcvids[nprocs];
  int swapids[nprocs];
  MPI_Status status;
  int steps;
  int istep;
  int rstep;
  int p;
  int maxreq;
  int maxreqh;
  int hs;
  char *ptr;

  offset_t = nprocs;
  // send to self
  if(sndlths[mytask] > 0){
    tag = mytask + offset_t;

    ptr = (char *) rcvbuf + rdispls[mytask];
    CheckMPIReturn(MPI_Irecv(ptr, rcvlths[mytask], rtypes[mytask], mytask, tag, comm, rcvids), __FILE__,__LINE__);     

    ptr = (char *) sndbuf + sdispls[mytask]; 
    CheckMPIReturn(MPI_Send(ptr, sndlths[mytask], stypes[mytask], mytask, tag, comm), __FILE__,__LINE__);
    CheckMPIReturn(MPI_Wait(rcvids, &status), __FILE__,__LINE__);

  }
  for(int i=0;i<nprocs;i++){
    swapids[i]=0;
    rcvids[i] = MPI_REQUEST_NULL;
    sndids[i]=MPI_REQUEST_NULL;
    hs_rcvids[i]=MPI_REQUEST_NULL;
  }

  steps = 0;
  for(istep=0;istep<ceil2(nprocs)-1;istep++){
    p = pair(nprocs, istep, mytask) ;
    if( p >= 0 && (sndlths[p] > 0 || rcvlths[p] > 0)){
      swapids[steps++] = p;
    }
  }

  if(steps == 0)
    return PIO_NOERR;
  
 
  if(steps == 1){
    maxreq = 1;
    maxreqh = 1;
  }else{
    if(max_requests > 1 && max_requests<steps){
      maxreq = max_requests;
      maxreqh = maxreq/2;
    }else if(max_requests>0){
      maxreq = 2;
      maxreqh = 1;
    }else{
      maxreq = steps;
      maxreqh = steps;
    }
  } 
  if(handshake){
    hs = 1;
    for(istep=0; istep<maxreq; istep++){
      p = swapids[istep];
      if( sndlths[p] > 0){
	tag = mytask+offset_t;
	CheckMPIReturn(MPI_Irecv( &hs, 1, MPI_INT, p, tag, comm, hs_rcvids+istep), __FILE__,__LINE__);
      }
    }
  }
  for(istep=0;istep < maxreq; istep++){
    p = swapids[istep];
    if(rcvlths[p] > 0){
      tag = p + offset_t;
      ptr = (char *) rcvbuf + rdispls[p];
      CheckMPIReturn(MPI_Irecv( ptr, rcvlths[p], rtypes[p], p, tag, comm, rcvids+istep), __FILE__,__LINE__);
      if(handshake)
	CheckMPIReturn(MPI_Send( &hs, 1, MPI_INT, p, tag, comm), __FILE__,__LINE__);
    }
  }

  rstep = maxreq;
  for(istep = 0; istep < steps; istep++){
    p = swapids[istep];
    if(sndlths[p] > 0){
      tag = mytask + offset_t;
      if(handshake)
	CheckMPIReturn(MPI_Wait ( hs_rcvids+istep, &status), __FILE__,__LINE__);
      
      ptr = (char *) sndbuf + sdispls[p];

      CheckMPIReturn(MPI_Rsend(ptr, sndlths[p], stypes[p], p, tag, comm), __FILE__,__LINE__);
    }
    if(istep > maxreqh){
      p = swapids[istep - maxreqh];
      if(rcvlths[p] > 0){
	CheckMPIReturn(MPI_Wait(rcvids+istep-maxreqh, &status), __FILE__,__LINE__);
      }
      if(rstep < steps){
	p = swapids[rstep];
	if(handshake && sndlths[p] > 0){
	  tag = mytask + offset_t;
	  CheckMPIReturn(MPI_Irecv( &hs, 1, MPI_INT, p, tag, comm, hs_rcvids+rstep), __FILE__,__LINE__);
	}
	if(rcvlths[p] > 0){
	  tag = p + offset_t;
	  
	  ptr = (char *) rcvbuf + rdispls[p];
	  
	  CheckMPIReturn(MPI_Irecv( ptr, rcvlths[p], rtypes[p], p, tag, comm, rcvids+rstep), __FILE__,__LINE__);
	  if(handshake)
	    CheckMPIReturn(MPI_Send( &hs, 1, MPI_INT, p, tag, comm), __FILE__,__LINE__);
	}
	rstep++;
      }
    }
  }
  for(istep=steps-maxreqh;istep<steps;istep++){
    p = swapids[istep];
    if(rcvlths[p] > 0)
      CheckMPIReturn(MPI_Wait( rcvids+istep, &status), __FILE__,__LINE__);
    if(isend && sndlths[p] > 0)
      CheckMPIReturn(MPI_Wait( sndids+istep, &status), __FILE__,__LINE__);
  }

  return PIO_NOERR;
}

#ifdef TESTSWAPM
#include <sys/time.h>
/*
This program tests MPI_Alltoallw by having processor i send different
amounts of data to each processor.
The first test sends i items to processor i from all processors.
*/
int main( int argc, char **argv )
{
    MPI_Comm comm;
    int *sbuf, *rbuf;
    int rank, size;
    int *sendcounts, *recvcounts, *rdispls, *sdispls;
    int i, j, *p, err;
    MPI_Datatype *sendtypes, *recvtypes;
    struct timeval t1, t2;


    MPI_Init( &argc, &argv );
    err = 0;
    comm = MPI_COMM_WORLD;
    /* Create the buffer */
    MPI_Comm_size( comm, &size );
    MPI_Comm_rank( comm, &rank );
    sbuf = (int *)malloc( size * size * sizeof(int) );
    rbuf = (int *)malloc( size * size * sizeof(int) );
    if (!sbuf || !rbuf) {
        fprintf( stderr, "Could not allocated buffers!\n" );
        fflush(stderr);
        MPI_Abort( comm, 1 );
    }

    /* Create and load the arguments to alltoallv */
    sendcounts = (int *)malloc( size * sizeof(int) );
    recvcounts = (int *)malloc( size * sizeof(int) );
    rdispls = (int *)malloc( size * sizeof(int) );
    sdispls = (int *)malloc( size * sizeof(int) );
    sendtypes = (MPI_Datatype *)malloc( size * sizeof(MPI_Datatype) );
    recvtypes = (MPI_Datatype *)malloc( size * sizeof(MPI_Datatype) );
    if (!sendcounts || !recvcounts || !rdispls || !sdispls || !sendtypes || !recvtypes) {
        fprintf( stderr, "Could not allocate arg items!\n" );
        fflush(stderr);
        MPI_Abort( comm, 1 );
    }

    for (i=0; i<size; i++) {
        sendcounts[i] = i + 1;
        recvcounts[i] = rank +1;
        rdispls[i] = i * (rank+1) * sizeof(int) ;
        sdispls[i] = (((i+1) * (i))/2) * sizeof(int) ;
        sendtypes[i] = recvtypes[i] = MPI_INT;
    }


    for(int itest=0;itest<5; itest++){
      bool hs; 
      bool isend;
      //      int msg_cnt = MAX_GATHER_BLOCK_SIZE;
      int msg_cnt = 1;

      /* Load up the buffers */
      for (i=0; i<size*size; i++) {
        sbuf[i] = i + 100*rank;
        rbuf[i] = -i;
      }
      MPI_Barrier(comm);

      if(rank==0) printf("Start itest %d\n",itest);
      if(rank == 0) gettimeofday(&t1, NULL);

      if(itest==0){
	MPI_Alltoallw( sbuf, sendcounts, sdispls, sendtypes,
		       rbuf, recvcounts, rdispls, recvtypes, comm );
      }else if(itest==1){
	hs = true;
	isend = true;
	err = pio_swapm( size, rank, sbuf, size*size, sendcounts, sdispls, sendtypes, 
			 rbuf, size*size, recvcounts, rdispls, recvtypes, comm, hs, isend, msg_cnt);
      }else if(itest==2){
	hs = false;
	isend = true;
	err = pio_swapm( size, rank, sbuf, size*size, sendcounts, sdispls, sendtypes, 
			 rbuf, size*size, recvcounts, rdispls, recvtypes, comm, hs, isend, msg_cnt);

      }else if(itest==3){
	hs = false;
	isend = false;
	err = pio_swapm( size, rank, sbuf, size*size, sendcounts, sdispls, sendtypes, 
			 rbuf, size*size, recvcounts, rdispls, recvtypes, comm, hs, isend, msg_cnt);

      }else if(itest==4){
	hs = true;
	isend = false;
	err = pio_swapm( size, rank, sbuf, size*size, sendcounts, sdispls, sendtypes, 
			 rbuf, size*size, recvcounts, rdispls, recvtypes, comm, hs, isend, msg_cnt);

      }	


      if(rank == 0){
	gettimeofday(&t2, NULL);
	printf("itest = %d time = %ld\n",itest,t2.tv_usec - t1.tv_usec);
      }
      /*
      printf("scnt: %d %d %d %d\n",sendcounts[0],sendcounts[1],sendcounts[2],sendcounts[3]);
      printf("sdispls: %d %d %d %d\n",sdispls[0],sdispls[1],sdispls[2],sdispls[3]);
      printf("rcnt: %d %d %d %d\n",recvcounts[0],recvcounts[1],recvcounts[2],recvcounts[3]);
      printf("rdispls: %d %d %d %d\n",rdispls[0],rdispls[1],rdispls[2],rdispls[3]);

      printf("send: ");
      for(i=0;i<size*size;i++)
	printf("%d ",sbuf[i]);
      printf("\n");
      printf("recv: ");
      for(i=0;i<size*size;i++)
	printf("%d ",rbuf[i]);
      printf("\n");
      */



      MPI_Barrier(comm);

      /* Check rbuf */
      for (i=0; i<size; i++) {
        p = rbuf + rdispls[i]/sizeof(int);
        for (j=0; j<rank+1; j++) {
	  if (p[j] != i * 100 + (rank*(rank+1))/2 + j) {
	    fprintf( stderr, "[%d] got %d expected %d for %d %dth in itest=%d\n",
		     rank, p[j],(i*100 + (rank*(rank+1))/2+j), i, j, itest);
	    fflush(stderr);
	    err++;
	  }
	}
      }

    }



    free( sendtypes );
    free( recvtypes );
    free( sdispls );
    free( rdispls );
    free( recvcounts );
    free( sendcounts );
    free( rbuf );
    free( sbuf );
    MPI_Finalize();
    return 0;
}


#endif