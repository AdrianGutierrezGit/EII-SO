#include "Simulator.h"
#include "OperatingSystem.h"
#include "OperatingSystemBase.h"
#include "MMU.h"
#include "Processor.h"
#include "Buses.h"
#include "Heap.h"
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <time.h>

// Functions prototypes
void OperatingSystem_PCBInitialization(int, int, int, int, int);
void OperatingSystem_MoveToTheREADYState(int);
void OperatingSystem_MoveToTheBLOCKEDState(int);
void OperatingSystem_Dispatch(int);
void OperatingSystem_RestoreContext(int);
void OperatingSystem_SaveContext(int);
void OperatingSystem_TerminateExecutingProcess();
int OperatingSystem_LongTermScheduler();
void OperatingSystem_PreemptRunningProcess();
void OperatingSystem_PreemptRunningProcessToBlocked();
int OperatingSystem_CreateProcess(int);
int OperatingSystem_ObtainMainMemory(int, int);
int OperatingSystem_ShortTermScheduler();
int OperatingSystem_ExtractFromReadyToRun();
void OperatingSystem_ExtractFromBlockedToReady();
void OperatingSystem_HandleException();
void OperatingSystem_HandleSystemCall();
void OperatingSystem_HandleSystemCallYield();
void OperatingSystem_HandleClockInterrupt();
void OperatingSystem_HandleSystemCallSleep();
void OperatingSystem_PrintReadyToRunQueue();

// The process table
// PCB processTable[PROCESSTABLEMAXSIZE];
PCB * processTable;

// Size of the memory occupied for the OS
int OS_MEMORY_SIZE=32;

// Address base for OS code in this version
int OS_address_base; 

// Identifier of the current executing process
int executingProcessID=NOPROCESS;

// Identifier of the System Idle Process
int sipID;

// Initial PID for assignation (Not assigned)
int initialPID=-1;

//numero de interrupciones de reloj ocurridas
int numberOfClockInterrupts = 0;

//estructura de datos para imprimir los cambios de los procesos
char * statesNames [5]={"NEW","READY","EXECUTING","BLOCKED","EXIT"};

// Begin indes for daemons in programList
// int baseDaemonsInProgramList; 

// Array that contains the identifiers of the READY processes
// heapItem readyToRunQueue[NUMBEROFQUEUES][PROCESSTABLEMAXSIZE];
heapItem *readyToRunQueue[NUMBEROFQUEUES];
//int numberOfReadyToRunProcesses[NUMBEROFQUEUES]={0};
int numberOfReadyToRunProcesses[NUMBEROFQUEUES]={0,0};
char * queueNames [NUMBEROFQUEUES]={"USER","DAEMONS"}; 

// In OperatingSystem.c Exercise 5-b of V2
// Heap with blocked processes sort by when to wakeup
heapItem *sleepingProcessesQueue;
int numberOfSleepingProcesses=0; 

// Variable containing the number of not terminated user processes
int numberOfNotTerminatedUserProcesses=0;

// char DAEMONS_PROGRAMS_FILE[MAXFILENAMELENGTH]="teachersDaemons";

int MAINMEMORYSECTIONSIZE = 60;

extern int MAINMEMORYSIZE;

int PROCESSTABLEMAXSIZE = 4;

// Initial set of tasks of the OS
void OperatingSystem_Initialize(int programsFromFileIndex) {
	
	int i, selectedProcess;
	FILE *programFile; // For load Operating System Code
	
// In this version, with original configuration of memory size (300) and number of processes (4)
// every process occupies a 60 positions main memory chunk 
// and OS code and the system stack occupies 60 positions 

	OS_address_base = MAINMEMORYSIZE - OS_MEMORY_SIZE;

	MAINMEMORYSECTIONSIZE = OS_address_base / PROCESSTABLEMAXSIZE;

	if (initialPID<0) // if not assigned in options...
		initialPID= PROCESSTABLEMAXSIZE - 1;
	
	// Space for the processTable
	processTable = (PCB *) malloc(PROCESSTABLEMAXSIZE*sizeof(PCB));
	
	// Space for the ready to run queues (one queue initially...)
	readyToRunQueue[USERPROCESSQUEUE] = Heap_create(PROCESSTABLEMAXSIZE);
	readyToRunQueue[DAEMONSQUEUE] = Heap_create(PROCESSTABLEMAXSIZE);
	sleepingProcessesQueue = Heap_create(PROCESSTABLEMAXSIZE);

	programFile=fopen("OperatingSystemCode", "r");
	if (programFile==NULL){
		// Show red message "FATAL ERROR: Missing Operating System!\n"
		ComputerSystem_DebugMessage(NO_TIMED_MESSAGE,99,SHUTDOWN,"FATAL ERROR: Missing Operating System!\n");
		exit(1);		
	}

	// Obtain the memory requirements of the program
	int processSize=OperatingSystem_ObtainProgramSize(programFile);

	// Load Operating System Code
	OperatingSystem_LoadProgram(programFile, OS_address_base, processSize);
	
	// Process table initialization (all entries are free)
	for (i=0; i<PROCESSTABLEMAXSIZE;i++){
		processTable[i].busy=0;
		processTable[i].initialPhysicalAddress=-1;
		processTable[i].processSize=-1;
		processTable[i].copyOfSPRegister=-1;
		processTable[i].state=-1;
		processTable[i].priority=-1;
		processTable[i].copyOfPCRegister=-1;
		processTable[i].copyOfPSWRegister=0;
		processTable[i].programListIndex=-1;
		processTable[i].copyOfAccumulator = 0;
		processTable[i].copyOfRegisterA = 0;
		processTable[i].copyOfRegisterB = 0;
		processTable[i].whenToWakeUp = -1;
	}
	// Initialization of the interrupt vector table of the processor
	Processor_InitializeInterruptVectorTable(OS_address_base+2);
		
	// Include in program list all user or system daemon processes
	OperatingSystem_PrepareDaemons(programsFromFileIndex);
	
	//Mete los programas en la cola de llegada
	ComputerSystem_FillInArrivalTimeQueue();

	OperatingSystem_PrintStatus();

	// Create all user processes from the information given in the command line
	OperatingSystem_LongTermScheduler();

	if (numberOfProgramsInArrivalTimeQueue == 0 && numberOfNotTerminatedUserProcesses == 0) {
		// Simulation must finish, telling sipID to finish
		OperatingSystem_ReadyToShutdown();
	}
	
	if (strcmp(programList[processTable[sipID].programListIndex]->executableName,"SystemIdleProcess")
		&& processTable[sipID].state==READY) {
		// Show red message "FATAL ERROR: Missing SIP program!\n"
		ComputerSystem_DebugMessage(NO_TIMED_MESSAGE,99,SHUTDOWN,"FATAL ERROR: Missing SIP program!\n");
		exit(1);		
	}

	// At least, one process has been created
	// Select the first process that is going to use the processor
	selectedProcess=OperatingSystem_ShortTermScheduler();

	Processor_SetSSP(MAINMEMORYSIZE-1);

	// Assign the processor to the selected process
	OperatingSystem_Dispatch(selectedProcess);

	// Initial operation for Operating System
	Processor_SetPC(OS_address_base);

	OperatingSystem_PrintStatus();

}

// The LTS is responsible of the admission of new processes in the system.
// Initially, it creates a process from each program specified in the 
// 			command line and daemons programs
int OperatingSystem_LongTermScheduler() {
  
	int PID, i,
		numberOfSuccessfullyCreatedProcesses=0;
	
	while(OperatingSystem_IsThereANewProgram() == YES){
		i = Heap_poll(arrivalTimeQueue, QUEUE_ARRIVAL, &numberOfProgramsInArrivalTimeQueue); 
		PID=OperatingSystem_CreateProcess(i);
	
		if(PID == NOFREEENTRY){
			ComputerSystem_DebugMessage(TIMED_MESSAGE,50,ERROR,programList[i]->executableName);
		}else if(PID == PROGRAMDOESNOTEXIST){
			ComputerSystem_DebugMessage(TIMED_MESSAGE,51,ERROR,programList[i]->executableName,"it does not exist");
		}else if(PID == PROGRAMNOTVALID){
			ComputerSystem_DebugMessage(TIMED_MESSAGE,51,ERROR,programList[i]->executableName,"invalid priority or size");
		}else if(PID == TOOBIGPROCESS){
			ComputerSystem_DebugMessage(TIMED_MESSAGE,52,ERROR,programList[i]->executableName);
		}else{
			numberOfSuccessfullyCreatedProcesses++;
			if (programList[i]->type==USERPROGRAM) 
				numberOfNotTerminatedUserProcesses++;
		// Move process to the ready state
			OperatingSystem_MoveToTheREADYState(PID);
		}
	}	
	if(numberOfSuccessfullyCreatedProcesses > 0){
		 OperatingSystem_PrintStatus();
	}
	// Return the number of succesfully created processes
	return numberOfSuccessfullyCreatedProcesses;
}

// This function creates a process from an executable program
int OperatingSystem_CreateProcess(int indexOfExecutableProgram) {
  
	int PID;
	int processSize;
	int loadingPhysicalAddress;
	int priority;
	FILE *programFile;
	PROGRAMS_DATA *executableProgram=programList[indexOfExecutableProgram];

	// Obtain a process ID
	PID=OperatingSystem_ObtainAnEntryInTheProcessTable();

	if(PID == NOFREEENTRY){
		return NOFREEENTRY;
	}

	// Check if programFile exists
	programFile=fopen(executableProgram->executableName, "r");
	if(programFile == NULL){
		return PROGRAMDOESNOTEXIST;
	}
	// Obtain the memory requirements of the program
	processSize=OperatingSystem_ObtainProgramSize(programFile);	

	if(processSize == PROGRAMNOTVALID){
		return PROGRAMNOTVALID;
	}
	// Obtain the priority for the process
	priority=OperatingSystem_ObtainPriority(programFile);

	if(priority == PROGRAMNOTVALID){
		return PROGRAMNOTVALID;
	}
	// Obtain enough memory space
 	loadingPhysicalAddress=OperatingSystem_ObtainMainMemory(processSize, PID);

	if(loadingPhysicalAddress == TOOBIGPROCESS){
		return TOOBIGPROCESS;
	}

	// Load program in the allocated memory
	int moreInstructionsthanSize = OperatingSystem_LoadProgram(programFile, loadingPhysicalAddress, processSize);
	if(moreInstructionsthanSize == TOOBIGPROCESS){
		return TOOBIGPROCESS;
	}
	// PCB initialization
	OperatingSystem_PCBInitialization(PID, loadingPhysicalAddress, processSize, priority, indexOfExecutableProgram);
	
	// Show message "Process [PID] created from program [executableName]\n"
	ComputerSystem_DebugMessage(TIMED_MESSAGE,54,SYSPROC,PID,"NEW",executableProgram->executableName);
	
	return PID;
}


// Main memory is assigned in chunks. All chunks are the same size. A process
// always obtains the chunk whose position in memory is equal to the processor identifier
int OperatingSystem_ObtainMainMemory(int processSize, int PID) {

 	if (processSize>MAINMEMORYSECTIONSIZE)
		return TOOBIGPROCESS;
	
 	return PID*MAINMEMORYSECTIONSIZE;
}


// Assign initial values to all fields inside the PCB
void OperatingSystem_PCBInitialization(int PID, int initialPhysicalAddress, int processSize, int priority, int processPLIndex) {

	processTable[PID].busy=1;
	processTable[PID].initialPhysicalAddress=initialPhysicalAddress;
	processTable[PID].processSize=processSize;
	processTable[PID].copyOfSPRegister=initialPhysicalAddress+processSize;
	processTable[PID].state=NEW;
	processTable[PID].priority=priority;
	processTable[PID].programListIndex=processPLIndex;
	processTable[PID].copyOfAccumulator = 0;
	processTable[PID].copyOfRegisterA = 0;
	processTable[PID].copyOfRegisterB = 0;
	processTable[PID].whenToWakeUp= -1;

	// Daemons run in protected mode and MMU use real address
	if (programList[processPLIndex]->type == DAEMONPROGRAM) {
		processTable[PID].queueID = DAEMONSQUEUE;
		processTable[PID].copyOfPCRegister=initialPhysicalAddress;
		processTable[PID].copyOfPSWRegister= ((unsigned int) 1) << EXECUTION_MODE_BIT;
	} 
	else {
		processTable[PID].queueID = USERPROCESSQUEUE;
		processTable[PID].copyOfPCRegister=0;
		processTable[PID].copyOfPSWRegister=0;
	}

}


// Move a process to the READY state: it will be inserted, depending on its priority, in
// a queue of identifiers of READY processes
void OperatingSystem_MoveToTheREADYState(int PID) {
	if(processTable[PID].state == EXECUTING){
		ComputerSystem_DebugMessage(TIMED_MESSAGE,53,SYSPROC,PID,programList[processTable[PID].programListIndex]->executableName,"EXECUTING","READY");
	}else if (processTable[PID].state == BLOCKED){
		ComputerSystem_DebugMessage(TIMED_MESSAGE,53,SYSPROC,PID,programList[processTable[PID].programListIndex]->executableName,"BLOCKED","READY");
	}else{
		ComputerSystem_DebugMessage(TIMED_MESSAGE,53,SYSPROC,PID,programList[processTable[PID].programListIndex]->executableName,"NEW","READY");
	}

	if(programList[processTable[PID].programListIndex]->type == DAEMONPROGRAM){
		if (Heap_add(PID, readyToRunQueue[DAEMONSQUEUE],QUEUE_PRIORITY ,&(numberOfReadyToRunProcesses[DAEMONSQUEUE]))>=0) {
			processTable[PID].state=READY;
		}
	}else{
		if (Heap_add(PID, readyToRunQueue[USERPROCESSQUEUE],QUEUE_PRIORITY ,&(numberOfReadyToRunProcesses[USERPROCESSQUEUE]))>=0) {
				processTable[PID].state=READY;
		}
	}
	//OperatingSystem_PrintReadyToRunQueue();
}

void OperatingSystem_MoveToTheBLOCKEDState(int PID){
	if (Heap_add(PID, sleepingProcessesQueue,QUEUE_WAKEUP ,&(numberOfSleepingProcesses))>=0) {
		processTable[PID].state=BLOCKED;
		ComputerSystem_DebugMessage(TIMED_MESSAGE,53,SYSPROC,PID,programList[processTable[PID].programListIndex]->executableName,"EXECUTING","BLOCKED");
	}
}



// The STS is responsible of deciding which process to execute when specific events occur.
// It uses processes priorities to make the decission. Given that the READY queue is ordered
// depending on processes priority, the STS just selects the process in front of the READY queue
int OperatingSystem_ShortTermScheduler() {
	
	int selectedProcess;

	selectedProcess=OperatingSystem_ExtractFromReadyToRun();
	
	return selectedProcess;
}


// Return PID of more priority process in the READY queue
int OperatingSystem_ExtractFromReadyToRun() {
  
	int selectedProcess=NOPROCESS;
	if(numberOfReadyToRunProcesses[USERPROCESSQUEUE] == 0){
		selectedProcess=Heap_poll(readyToRunQueue[DAEMONSQUEUE],QUEUE_PRIORITY ,&(numberOfReadyToRunProcesses[DAEMONSQUEUE]));
	}else{
		selectedProcess=Heap_poll(readyToRunQueue[USERPROCESSQUEUE],QUEUE_PRIORITY ,&(numberOfReadyToRunProcesses[USERPROCESSQUEUE]));
	}	
	// Return most priority process or NOPROCESS if empty queue
	return selectedProcess; 
}

void OperatingSystem_ExtractFromBlockedToReady(){
	int selectedProcess = NOPROCESS;
	int actualNumberOfSleepingProcess = numberOfSleepingProcesses;
	for(int i = 0; i < actualNumberOfSleepingProcess; i++){
		if(processTable[sleepingProcessesQueue[i].info].whenToWakeUp == numberOfClockInterrupts){
			selectedProcess = sleepingProcessesQueue[i].info;
			Heap_poll(sleepingProcessesQueue,QUEUE_WAKEUP,&(numberOfSleepingProcesses));
			OperatingSystem_MoveToTheREADYState(selectedProcess);
			OperatingSystem_PrintStatus();
		}
	}
	int processesCreated = OperatingSystem_LongTermScheduler();
	if(numberOfProgramsInArrivalTimeQueue == 0 && numberOfNotTerminatedUserProcesses == 0){
		OperatingSystem_ReadyToShutdown();
	}
	if(processesCreated > 0 || selectedProcess != NOPROCESS){
		int processToExecuted = NOPROCESS; 
		if(programList[processTable[executingProcessID].programListIndex]->type == DAEMONPROGRAM){
			if((processTable[readyToRunQueue[DAEMONSQUEUE][0].info].priority < processTable[executingProcessID].priority && numberOfReadyToRunProcesses[USERPROCESSQUEUE] == 0)){
				ComputerSystem_DebugMessage(TIMED_MESSAGE,58,SHORTTERMSCHEDULE,executingProcessID,programList[processTable[executingProcessID].programListIndex]->executableName,readyToRunQueue[DAEMONSQUEUE][0].info,programList[processTable[readyToRunQueue[DAEMONSQUEUE][0].info].programListIndex]-> executableName);
				OperatingSystem_PreemptRunningProcess();
				processToExecuted = OperatingSystem_ShortTermScheduler();
				OperatingSystem_Dispatch(processToExecuted);
				OperatingSystem_PrintStatus();
			}else if(numberOfReadyToRunProcesses[USERPROCESSQUEUE] != 0){
				ComputerSystem_DebugMessage(TIMED_MESSAGE,58,SHORTTERMSCHEDULE,executingProcessID,programList[processTable[executingProcessID].programListIndex]->executableName,readyToRunQueue[USERPROCESSQUEUE][0].info,programList[processTable[readyToRunQueue[USERPROCESSQUEUE][0].info].programListIndex]-> executableName);
				OperatingSystem_PreemptRunningProcess();
				processToExecuted = OperatingSystem_ShortTermScheduler();
				OperatingSystem_Dispatch(processToExecuted);
				OperatingSystem_PrintStatus();
			}
		} else if(processTable[readyToRunQueue[USERPROCESSQUEUE][0].info].priority < processTable[executingProcessID].priority){
			ComputerSystem_DebugMessage(TIMED_MESSAGE,58,SHORTTERMSCHEDULE,executingProcessID,programList[processTable[executingProcessID].programListIndex]->executableName,readyToRunQueue[USERPROCESSQUEUE][0].info,programList[processTable[readyToRunQueue[USERPROCESSQUEUE][0].info].programListIndex]-> executableName);
			OperatingSystem_PreemptRunningProcess();
			processToExecuted = OperatingSystem_ShortTermScheduler();
			OperatingSystem_Dispatch(processToExecuted);
			OperatingSystem_PrintStatus();
		}
	}
	
}


// Function that assigns the processor to a process
void OperatingSystem_Dispatch(int PID) {

	// The process identified by PID becomes the current executing process
	executingProcessID=PID;
	// Change the process' state
	processTable[PID].state=EXECUTING;
	ComputerSystem_DebugMessage(TIMED_MESSAGE,53,SYSPROC,PID,programList[processTable[PID].programListIndex]->executableName,"READY","EXECUTING");
	// Modify hardware registers with appropriate values for the process identified by PID
	OperatingSystem_RestoreContext(PID);
}


// Modify hardware registers with appropriate values for the process identified by PID
void OperatingSystem_RestoreContext(int PID) {
  
	// New values for the CPU registers are obtained from the PCB
	Processor_PushInSystemStack(processTable[PID].copyOfPCRegister);
	Processor_PushInSystemStack(processTable[PID].copyOfPSWRegister);
	Processor_SetRegisterSP(processTable[PID].copyOfSPRegister);
	Processor_SetAccumulator(processTable[PID].copyOfAccumulator);
	Processor_SetRegisterA(processTable[PID].copyOfRegisterA);
	Processor_SetRegisterB(processTable[PID].copyOfRegisterB);

	// Same thing for the MMU registers
	MMU_SetBase(processTable[PID].initialPhysicalAddress);
	MMU_SetLimit(processTable[PID].processSize);
}


// Function invoked when the executing process leaves the CPU 
void OperatingSystem_PreemptRunningProcess() {

	// Save in the process' PCB essential values stored in hardware registers and the system stack
	OperatingSystem_SaveContext(executingProcessID);
	// Change the process' state
	OperatingSystem_MoveToTheREADYState(executingProcessID);
	// The processor is not assigned until the OS selects another process
	executingProcessID=NOPROCESS;
}

void OperatingSystem_PreemptRunningProcessToBlocked(){
	// Save in the process' PCB essential values stored in hardware registers and the system stack
	OperatingSystem_SaveContext(executingProcessID);
	// Change the process' state
	OperatingSystem_MoveToTheBLOCKEDState(executingProcessID);
	// The processor is not assigned until the OS selects another process
	executingProcessID=NOPROCESS;
}

// Save in the process' PCB essential values stored in hardware registers and the system stack
void OperatingSystem_SaveContext(int PID) {
	
	// Load PSW saved for interrupt manager
	processTable[PID].copyOfPSWRegister=Processor_PopFromSystemStack();
	
	// Load PC saved for interrupt manager
	processTable[PID].copyOfPCRegister=Processor_PopFromSystemStack();
	
	// Save RegisterSP 
	processTable[PID].copyOfSPRegister=Processor_GetRegisterSP();

	//Salvamos el valor del acumlador
	processTable[PID].copyOfAccumulator = Processor_GetAccumulator();

	//Salvamos el valor del registro A
	processTable[PID].copyOfRegisterA = Processor_GetRegisterA();

	//Salvamos el valor del registro B
	processTable[PID].copyOfRegisterB = Processor_GetRegisterB();
}


// Exception management routine
void OperatingSystem_HandleException() {
  
	// Show message "Process [executingProcessID] has generated an exception and is terminating\n"
	ComputerSystem_DebugMessage(TIMED_MESSAGE,71,INTERRUPT,executingProcessID,programList[processTable[executingProcessID].programListIndex]->executableName);
	
	OperatingSystem_TerminateExecutingProcess();
	OperatingSystem_PrintStatus();
}

// All tasks regarding the removal of the executing process
void OperatingSystem_TerminateExecutingProcess() {

	processTable[executingProcessID].state=EXIT;
	ComputerSystem_DebugMessage(TIMED_MESSAGE,53,SYSPROC,executingProcessID,programList[processTable[executingProcessID].programListIndex]->executableName,"EXECUTING","EXIT");
	
	if (executingProcessID==sipID) {
		// finishing sipID, change PC to address of OS HALT instruction
		Processor_SetSSP(MAINMEMORYSIZE-1);
		Processor_PushInSystemStack(OS_address_base+1);
		Processor_PushInSystemStack(Processor_GetPSW());
		executingProcessID=NOPROCESS;
		ComputerSystem_DebugMessage(TIMED_MESSAGE,99,SHUTDOWN,"The system will shut down now...\n");
		return; // Don't dispatch any process
	}

	Processor_SetSSP(Processor_GetSSP()+2); // unstack PC and PSW stacked

	if (programList[processTable[executingProcessID].programListIndex]->type==USERPROGRAM) 
		// One more user process that has terminated
		numberOfNotTerminatedUserProcesses--;
	
	if (numberOfProgramsInArrivalTimeQueue == 0 && numberOfNotTerminatedUserProcesses == 0) {
		// Simulation must finish, telling sipID to finish
		OperatingSystem_ReadyToShutdown();
	}
	// Select the next process to execute (sipID if no more user processes)
	int selectedProcess=OperatingSystem_ShortTermScheduler();
	// Assign the processor to that process
	OperatingSystem_Dispatch(selectedProcess);
}

// System call management routine
void OperatingSystem_HandleSystemCall() {
  
	int systemCallID;

	// Register A contains the identifier of the issued system call
	systemCallID=Processor_GetRegisterC();
	
	switch (systemCallID) {
		case SYSCALL_PRINTEXECPID:
			// Show message: "Process [executingProcessID] has the processor assigned\n"
			ComputerSystem_DebugMessage(TIMED_MESSAGE,72,SYSPROC,executingProcessID,programList[processTable[executingProcessID].programListIndex]->executableName,Processor_GetRegisterA(),Processor_GetRegisterB());
			break;

		case SYSCALL_END:
			// Show message: "Process [executingProcessID] has requested to terminate\n"
			ComputerSystem_DebugMessage(TIMED_MESSAGE,73,SYSPROC,executingProcessID,programList[processTable[executingProcessID].programListIndex]->executableName);
			OperatingSystem_TerminateExecutingProcess();
			OperatingSystem_PrintStatus();
			break;
		case SYSCALL_YIELD:
			OperatingSystem_HandleSystemCallYield();
			break;
		case SYSCALL_SLEEP:
			OperatingSystem_HandleSystemCallSleep();
			break;
	}
}
	
//	Implement interrupt logic calling appropriate interrupt handle
void OperatingSystem_InterruptLogic(int entryPoint){
	switch (entryPoint){
		case SYSCALL_BIT: // SYSCALL_BIT=2
			OperatingSystem_HandleSystemCall();
			break;
		case EXCEPTION_BIT: // EXCEPTION_BIT=6
			OperatingSystem_HandleException();
			break;
		case CLOCKINT_BIT:
			OperatingSystem_HandleClockInterrupt();
			break;
	}

}

void OperatingSystem_HandleSystemCallSleep(){
	int operando2 = Processor_GetRegisterD();
	int retardo;
	if(operando2 > 0)
		retardo = operando2;
	else
		retardo = abs(Processor_GetAccumulator());
	processTable[executingProcessID].whenToWakeUp = retardo+numberOfClockInterrupts+1;
	OperatingSystem_PreemptRunningProcessToBlocked();
	int selectedProcess = OperatingSystem_ShortTermScheduler();
	OperatingSystem_Dispatch(selectedProcess);
	OperatingSystem_PrintStatus();
}

void OperatingSystem_HandleClockInterrupt(){ 
	numberOfClockInterrupts++;
	ComputerSystem_DebugMessage(TIMED_MESSAGE,57,INTERRUPT,numberOfClockInterrupts); 
	OperatingSystem_ExtractFromBlockedToReady();
}
 

void OperatingSystem_HandleSystemCallYield(){
	int firstReadyProcess;
	if(programList[processTable[executingProcessID].programListIndex]->type == USERPROGRAM){
		firstReadyProcess = Heap_getFirst(readyToRunQueue[USERPROCESSQUEUE],numberOfReadyToRunProcesses[USERPROCESSQUEUE]);
	}else{
		firstReadyProcess = Heap_getFirst(readyToRunQueue[DAEMONSQUEUE],numberOfReadyToRunProcesses[DAEMONSQUEUE]);
	}
	if((processTable[executingProcessID].priority - processTable[firstReadyProcess].priority == 0)){
		ComputerSystem_DebugMessage(TIMED_MESSAGE,55,SHORTTERMSCHEDULE,executingProcessID,programList[processTable[executingProcessID].programListIndex]->executableName,
		firstReadyProcess,programList[processTable[firstReadyProcess].programListIndex]->executableName);
		OperatingSystem_PreemptRunningProcess();
		int newProcessToRun = OperatingSystem_ShortTermScheduler();
		OperatingSystem_Dispatch(newProcessToRun);
		OperatingSystem_PrintStatus();
	}else{
		ComputerSystem_DebugMessage(TIMED_MESSAGE,56,SHORTTERMSCHEDULE,executingProcessID,programList[processTable[executingProcessID].programListIndex]->executableName);
	}
}

void OperatingSystem_PrintReadyToRunQueue(){
	ComputerSystem_DebugMessage(TIMED_MESSAGE,103,SHORTTERMSCHEDULE);
	if(numberOfReadyToRunProcesses[USERPROCESSQUEUE] == 0){
		ComputerSystem_DebugMessage(NO_TIMED_MESSAGE,100,SHORTTERMSCHEDULE, "\t\tUSER:\n");
	}else{
		ComputerSystem_DebugMessage(NO_TIMED_MESSAGE,100,SHORTTERMSCHEDULE, "\t\tUSER:");
	}
	for(int i = 0; i < numberOfReadyToRunProcesses[USERPROCESSQUEUE]; i++){
		if(i == numberOfReadyToRunProcesses[USERPROCESSQUEUE] - 1){
			ComputerSystem_DebugMessage(NO_TIMED_MESSAGE,104,SHORTTERMSCHEDULE,readyToRunQueue[USERPROCESSQUEUE][i].info,processTable[readyToRunQueue[USERPROCESSQUEUE][i].info].priority);
		}else{
			ComputerSystem_DebugMessage(NO_TIMED_MESSAGE,105,SHORTTERMSCHEDULE,readyToRunQueue[USERPROCESSQUEUE][i].info,processTable[readyToRunQueue[USERPROCESSQUEUE][i].info].priority);
		}
	}
	if(numberOfReadyToRunProcesses[DAEMONSQUEUE] == 0){
		ComputerSystem_DebugMessage(NO_TIMED_MESSAGE,100,SHORTTERMSCHEDULE, "\t\tDAEMONS:\n");
	}else{
		ComputerSystem_DebugMessage(NO_TIMED_MESSAGE,100,SHORTTERMSCHEDULE, "\t\tDAEMONS:");
	}
	for(int i = 0; i < numberOfReadyToRunProcesses[DAEMONSQUEUE];i++){
		if(i == numberOfReadyToRunProcesses[DAEMONSQUEUE] - 1){
			ComputerSystem_DebugMessage(NO_TIMED_MESSAGE,104,SHORTTERMSCHEDULE,readyToRunQueue[DAEMONSQUEUE][i].info,processTable[readyToRunQueue[DAEMONSQUEUE][i].info].priority);
		}else{
			ComputerSystem_DebugMessage(NO_TIMED_MESSAGE,105,SHORTTERMSCHEDULE,readyToRunQueue[DAEMONSQUEUE][i].info,processTable[readyToRunQueue[DAEMONSQUEUE][i].info].priority);
		}
	}
}



