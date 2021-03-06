/***********************************************************************/
/***********************************************************************
 Pipeline Cache Simulator
 ***********************************************************************/
/***********************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <math.h>

#define MAX_CACHE_SIZE 10240
#define CACHE_MISS_DELAY 10 // 10 cycle cache miss penalty
#define MAX_STAGES 5

// init the simulator
void iplc_sim_init(int index, int blocksize, int assoc);

// Cache simulator functions
void iplc_sim_LRU_replace_on_miss(int index, int tag);
void iplc_sim_LRU_update_on_hit(int index, int assoc);
int iplc_sim_trap_address(unsigned int address);

// Pipeline functions
unsigned int iplc_sim_parse_reg(char *reg_str);
void iplc_sim_parse_instruction(char *buffer);
void iplc_sim_push_pipeline_stage();
void iplc_sim_process_pipeline_rtype(char *instruction, int dest_reg,
                                     int reg1, int reg2_or_constant);
void iplc_sim_process_pipeline_lw(int dest_reg, int base_reg, unsigned int data_address);
void iplc_sim_process_pipeline_sw(int src_reg, int base_reg, unsigned int data_address);
void iplc_sim_process_pipeline_branch(int reg1, int reg2);
void iplc_sim_process_pipeline_jump();
void iplc_sim_process_pipeline_syscall();
void iplc_sim_process_pipeline_nop();

// Outout performance results
void iplc_sim_finalize();

typedef struct assoc_set
{
  // Due to 2-way and 4-way set asasociativity, cache lines could have
  // sets that can fit multiple blocks in them.
  int valid;
  int tag;
} set_t;

typedef struct cache_line
{
    // Your data structures for implementing your cache should include:
    // a valid bit
    // a tag
    // a method for handling varying levels of associativity
    // a method for selecting which item in the cache is going to be replaced
    set_t* sets;
    unsigned int* replacements;
} cache_line_t;

cache_line_t *cache=NULL;
int cache_index=0;
int cache_blocksize=0;
int cache_blockoffsetbits = 0;
int cache_assoc=0;
long cache_miss=0;
long cache_access=0;
long cache_hit=0;

char instruction[16];
char reg1[16];
char reg2[16];
char offsetwithreg[16];
unsigned int data_address=0;
unsigned int instruction_address=0;
unsigned int pipeline_cycles=0;   // how many cycles did your pipeline consume
unsigned int instruction_count=0; // how many real instructions ran thru the pipeline
unsigned int branch_predict_taken=0;
unsigned int branch_count=0;
unsigned int correct_branch_predictions=0;

unsigned int debug=0;
unsigned int dump_pipeline=1;

enum instruction_type {NOP, RTYPE, LW, SW, BRANCH, JUMP, JAL, SYSCALL};

typedef struct rtype
{
    char instruction[16];
    int reg1;
    int reg2_or_constant;
    int dest_reg;

} rtype_t;

typedef struct load_word
{
    unsigned int data_address;
    int dest_reg;
    int base_reg;

} lw_t;

typedef struct store_word
{
    unsigned int data_address;
    int src_reg;
    int base_reg;
} sw_t;

typedef struct branch
{
    int reg1;
    int reg2;

} branch_t;

typedef struct jump
{
    char instruction[16];

} jump_t;

typedef struct pipeline
{
    enum instruction_type itype;
    unsigned int instruction_address;
    union
    {
        rtype_t   rtype;
        lw_t      lw;
        sw_t      sw;
        branch_t  branch;
        jump_t    jump;
    }
    stage;

} pipeline_t;

enum pipeline_stages {FETCH, DECODE, ALU, MEM, WRITEBACK};

pipeline_t pipeline[MAX_STAGES];

/************************************************************************************************/
/* Cache Functions ******************************************************************************/
/************************************************************************************************/
/*
 * Correctly configure the cache.
 */
void iplc_sim_init(int index, int blocksize, int assoc)
{
    int i=0, j=0;
    unsigned long cache_size = 0;
    cache_index = index;
    cache_blocksize = blocksize;
    cache_assoc = assoc;


    cache_blockoffsetbits =
    (int) rint((log( (double) (blocksize * 4) )/ log(2)));
    /* Note: rint function rounds the result up prior to casting */

    cache_size = assoc * ( 1 << index ) * ((32 * blocksize) + 33 - index - cache_blockoffsetbits);

    printf("Cache Configuration \n");
    printf("   Index: %d bits or %d lines \n", cache_index, (1<<cache_index) );
    printf("   BlockSize: %d \n", cache_blocksize );
    printf("   Associativity: %d \n", cache_assoc );
    printf("   BlockOffSetBits: %d \n", cache_blockoffsetbits );
    printf("   CacheSize: %lu \n", cache_size );

    if (cache_size > MAX_CACHE_SIZE ) {
        printf("Cache too big. Great than MAX SIZE of %d .... \n", MAX_CACHE_SIZE);
        exit(-1);
    }

    cache = (cache_line_t *) malloc((sizeof(cache_line_t) * 1<<index));

    // Dynamically create our cache based on the information the user entered
    for (; i < (1<<index); i++) {
      cache[i].replacements = (unsigned int*) malloc(sizeof(unsigned int) * assoc);
      cache[i].sets = (set_t*) malloc(sizeof(set_t) * assoc);
      for (; j < assoc; j++) {
        cache[i].sets[j].valid = 0;
        cache[i].sets[j].tag = 0;
        cache[i].replacements[j] = j;
      }
    }

    // init the pipeline -- set all data to zero and instructions to NOP
    for (i = 0; i < MAX_STAGES; i++) {
        // itype is set to O which is NOP type instruction
        bzero(&(pipeline[i]), sizeof(pipeline_t));
    }
}

/*
 * iplc_sim_trap_address() determined this is not in our cache.  Put it there
 * and make sure that is now our Most Recently Used (MRU) entry.
 */
void iplc_sim_LRU_replace_on_miss(int index, int tag)
{
    int i=0;

    // Iterate through items to shift them forward.
    for (; i < cache_assoc - 1; i++) {
      cache[index].sets[i] = cache[index].sets[i+1];
      cache[index].replacements[i] = cache[index].replacements[i+1];
    }

    // Replace MRU entry
    cache[index].sets[cache_assoc-1].valid = 1;
    cache[index].sets[cache_assoc-1].tag = tag;
    cache[index].replacements[cache_assoc-1] = 0;
}

/*
 * iplc_sim_trap_address() determined the entry is in our cache.  Update its
 * information in the cache.
 */
void iplc_sim_LRU_update_on_hit(int index, int assoc_entry)
{
    int i=0, j=0;

    // Iterate through items to shift them backwards.
    for (i = assoc_entry; i > 0; i--) {
        cache[index].replacements[i] = cache[index].replacements[i-1];
    }

    // LRU is replaced with entry in cache
    cache[index].replacements[0] = cache[index].replacements[assoc_entry];
}

/*
 * Check if the address is in our cache.  Update our counter statistics
 * for cache_access, cache_hit, etc.  If our configuration supports
 * associativity we may need to check through multiple entries for our
 * desired index.  In that case we will also need to call the LRU functions.
 */
int iplc_sim_trap_address(unsigned int address)
{
    int i=0, index=0;
    int tag=0;
    int hit=0;

    // Index prepared using mask, tag is collected using combination of index and BOB
    cache_access++;
    index = address >> cache_blockoffsetbits & ((1 << cache_index) - 1);
    tag = address >> (cache_index + cache_blockoffsetbits);

    for (; i < cache_assoc; i++) {
      if (cache[index].sets[i].tag == tag) {
        hit = 1; // hit, use prepared method
        iplc_sim_LRU_update_on_hit(index, i);
        cache_hit++;
        return hit;
      }
    }

    // miss, use prepared method
    iplc_sim_LRU_replace_on_miss(index, tag);
    cache_miss++;

    /* expects you to return 1 for hit, 0 for miss */
    return hit;
}

/*
 * Just output our summary statistics.
 */
void iplc_sim_finalize()
{
    /* Finish processing all instructions in the Pipeline */
    while (pipeline[FETCH].itype != NOP  ||
           pipeline[DECODE].itype != NOP ||
           pipeline[ALU].itype != NOP    ||
           pipeline[MEM].itype != NOP    ||
           pipeline[WRITEBACK].itype != NOP) {
        iplc_sim_push_pipeline_stage();
    }

    printf(" Cache Performance \n");
    printf("\t Number of Cache Accesses is %ld \n", cache_access);
    printf("\t Number of Cache Misses is %ld \n", cache_miss);
    printf("\t Number of Cache Hits is %ld \n", cache_hit);
    printf("\t Cache Miss Rate is %f \n\n", (double)cache_miss / (double)cache_access);
    printf("Pipeline Performance \n");
    printf("\t Total Cycles is %u \n", pipeline_cycles);
    printf("\t Total Instructions is %u \n", instruction_count);
    printf("\t Total Branch Instructions is %u \n", branch_count);
    printf("\t Total Correct Branch Predictions is %u \n", correct_branch_predictions);
    printf("\t CPI is %f \n\n", (double)pipeline_cycles / (double)instruction_count);
}

/************************************************************************************************/
/* Pipeline Functions ***************************************************************************/
/************************************************************************************************/

/*
 * Dump the current contents of our pipeline.
 */
void iplc_sim_dump_pipeline() //DONE
{
    int i;

    for (i = 0; i < MAX_STAGES; i++) {
        switch(i) {
            case FETCH:
                printf("(cyc: %u) FETCH:\t %d: 0x%x \t", pipeline_cycles, pipeline[i].itype, pipeline[i].instruction_address);
                break;
            case DECODE:
                printf("DECODE:\t %d: 0x%x \t", pipeline[i].itype, pipeline[i].instruction_address);
                break;
            case ALU:
                printf("ALU:\t %d: 0x%x \t", pipeline[i].itype, pipeline[i].instruction_address);
                break;
            case MEM:
                printf("MEM:\t %d: 0x%x \t", pipeline[i].itype, pipeline[i].instruction_address);
                break;
            case WRITEBACK:
                printf("WB:\t %d: 0x%x \n", pipeline[i].itype, pipeline[i].instruction_address);
                break;
            default:
                printf("DUMP: Bad stage!\n" );
                exit(-1);
        }
    }
}

/*
 * Check if various stages of our pipeline require stalls, forwarding, etc.
 * Then push the contents of our various pipeline stages through the pipeline.
 */
void iplc_sim_push_pipeline_stage() //TYLER
{
    int i;
    int data_hit=1;

    /* 1. Count WRITEBACK stage is "retired" -- This I'm giving you */
    if (pipeline[WRITEBACK].instruction_address) {
        instruction_count++;
        if (debug)
            printf("DEBUG: Retired Instruction at 0x%x, Type %d, at Time %u \n",
                   pipeline[WRITEBACK].instruction_address, pipeline[WRITEBACK].itype, pipeline_cycles);
    }

    /* 2. Check for BRANCH and correct/incorrect Branch Prediction */
    if (pipeline[DECODE].itype == BRANCH)
    {
    	branch_count++; //hey, I found a branch!
        int branch_taken = 0;
        if(pipeline[FETCH].instruction_address!=0 && (pipeline[FETCH].instruction_address -pipeline[DECODE].instruction_address != 4))
        {
        	branch_taken = 1;
        }
        //if the branch is not correctly predicted, add one cycle, push stages through (except for decode), and insert a nop
		if (pipeline[FETCH].instruction_address != 0 && branch_taken != branch_predict_taken)
		{
			pipeline_cycles++;

			memcpy(&pipeline[WRITEBACK], &pipeline[MEM], sizeof(pipeline_t)); //MEM->WB
			memcpy(&pipeline[MEM], &pipeline[ALU], sizeof(pipeline_t));	//ALU->MEM
			memcpy(&pipeline[ALU], &pipeline[DECODE], sizeof(pipeline_t));//DECODE->ALU

			if (pipeline[WRITEBACK].instruction_address)
			{
				instruction_count++;
			}

			bzero(&(pipeline[DECODE]), sizeof(pipeline_t)); //this is where the NOP goes
		}
		//if the instruction address exists and is all dandy, then you correctly predicted a branch. Congrats!
		else if (pipeline[FETCH].instruction_address != 0)
		{
			correct_branch_predictions++;
		}
    }


    /* 3. Check for LW delays due to use in ALU stage and if data hit/miss
     *    add delay cycles if needed.
     */
    if (pipeline[MEM].itype == LW)
    {
        int inserted_nop = 0;

		//is the data in the cache?
		data_hit = iplc_sim_trap_address(pipeline[MEM].stage.lw.data_address);

		if (data_hit)
		{
			//if the data is in the cache, it's a hit. Print that.
			printf("DATA HIT:\t Address 0x%x \n", pipeline[MEM].stage.lw.data_address);
		}
		else
		{
			//if not, it's a miss. Print that.
			printf("DATA MISS:\t Address 0x%x \n", pipeline[MEM].stage.lw.data_address);

			//cache missing has a delay, so we add almost all of those cycles here (one is still added in Step 5)
			pipeline_cycles += CACHE_MISS_DELAY - 1;
		}

		//check if the ALU stage is an r-type instruction
		//this could cause some memory conflicts
		if (pipeline[ALU].itype == RTYPE)
		{
			//if so, we need to check more
			int instructionLength = strlen(pipeline[ALU].stage.rtype.instruction);
			// Is either reg in the ALU stage being used in the MEM stage?
			if ((pipeline[ALU].stage.rtype.reg1 == pipeline[MEM].stage.lw.dest_reg) || ((pipeline[ALU].stage.rtype.reg2_or_constant == pipeline[MEM].stage.lw.dest_reg) && pipeline[ALU].stage.rtype.instruction[instructionLength - 1] != 'i'))
			{
				pipeline_cycles++; //tentatively add the cycle

				//Moving the stuff from MEM into WB, to make room
				memcpy(&pipeline[WRITEBACK], &pipeline[MEM], sizeof(pipeline_t));

				//adding the NOP here
				bzero(&(pipeline[MEM]), sizeof(pipeline_t));
				inserted_nop = 1;

				if (pipeline[WRITEBACK].instruction_address)
				{
					instruction_count++;
				}
			}
		}
		if (!data_hit && inserted_nop)
		{
			pipeline_cycles--; //we didn't actually take that cycle, so...
		}
    }


    /* 4. Check for SW mem access and data miss and add delay cycles if needed */
    if (pipeline[MEM].itype == SW)
    {
        //Similar to step 3, is the data in the cache?
        data_hit = iplc_sim_trap_address(pipeline[MEM].stage.sw.data_address);

        if(data_hit)
        {
        	printf("DATA HIT:\t Address 0x%x \n",pipeline[MEM].stage.sw.data_address);
        }
        else
        {
        	//if we miss, print it
        	printf("DATA MISS:\t Address 0x%x \n",pipeline[MEM].stage.sw.data_address);

        	//and we need to add almost all of the miss delay, except for the one cycle in Step 5 below.
            pipeline_cycles += CACHE_MISS_DELAY - 1;

        }
    }


    /* 5. Increment pipe_cycles 1 cycle for normal processing */
    pipeline_cycles++;


    /* 6. push stages thru MEM->WB, ALU->MEM, DECODE->ALU, FETCH->DECODE */

    //let me re-write that as: FETCH->DECODE->ALU->MEM->WB
    //working backwards is the best way to avoid losing data

    memcpy(&pipeline[WRITEBACK], &pipeline[MEM], sizeof(pipeline_t)); 	//MEM->WB
    memcpy(&pipeline[MEM], &pipeline[ALU], sizeof(pipeline_t));			//ALU->MEM
    memcpy(&pipeline[ALU], &pipeline[DECODE], sizeof(pipeline_t));		//DECODE->ALU
    memcpy(&pipeline[DECODE], &pipeline[FETCH], sizeof(pipeline_t));	//FETCH->DECODE

    //...and there's nothing prior to FETCH to put in, so we move on to step 7


    // 7. This is a give'me -- Reset the FETCH stage to NOP via bezero */
    bzero(&(pipeline[FETCH]), sizeof(pipeline_t));
}

/*
 * This function is fully implemented.  You should use this as a reference
 * for implementing the remaining instruction types.
 */
void iplc_sim_process_pipeline_rtype(char *instruction, int dest_reg, int reg1, int reg2_or_constant) //DONE
{
    /* This is an example of what you need to do for the rest */ //Hi yes I'm writing in here to template stuff for myself
    iplc_sim_push_pipeline_stage(); //Step 1: push stage

    pipeline[FETCH].itype = RTYPE; //Step 2: set itype and instruction_address. This is the same among ALL instructions
    pipeline[FETCH].instruction_address = instruction_address;

    strcpy(pipeline[FETCH].stage.rtype.instruction, instruction); //Step 3: set instruction-specific variables. These are different between.
    pipeline[FETCH].stage.rtype.reg1 = reg1;
    pipeline[FETCH].stage.rtype.reg2_or_constant = reg2_or_constant;
    pipeline[FETCH].stage.rtype.dest_reg = dest_reg;
}

void iplc_sim_process_pipeline_lw(int dest_reg, int base_reg, unsigned int data_address) //TYLER
{
	iplc_sim_push_pipeline_stage(); //Step 1: push stage

	pipeline[FETCH].itype = LW;		//Step 2: set itype and address
	pipeline[FETCH].instruction_address = instruction_address;

	pipeline[FETCH].stage.lw.base_reg = base_reg; //step 3: Copy specific variables/arguments
	pipeline[FETCH].stage.lw.dest_reg = dest_reg;
	pipeline[FETCH].stage.lw.data_address = data_address;
    /* You must implement this function */
}

void iplc_sim_process_pipeline_sw(int src_reg, int base_reg, unsigned int data_address) //TYLER
{
	iplc_sim_push_pipeline_stage(); //Step 1: push stage

	pipeline[FETCH].itype = SW;		//Step 2: set itype and address
	pipeline[FETCH].instruction_address = instruction_address;

	pipeline[FETCH].stage.sw.base_reg = base_reg; //step 3: Copy specific variables/arguments
	pipeline[FETCH].stage.sw.src_reg = src_reg;
	pipeline[FETCH].stage.sw.data_address = data_address;
    /* You must implement this function */
}

void iplc_sim_process_pipeline_branch(int reg1, int reg2) //TYLER
{
	iplc_sim_push_pipeline_stage(); //Step 1: push stage

	pipeline[FETCH].itype = BRANCH;		//Step 2: set itype and address
	pipeline[FETCH].instruction_address = instruction_address;

	pipeline[FETCH].stage.branch.reg1 = reg1; //step 3: Copy specific variables/arguments
	pipeline[FETCH].stage.branch.reg2 = reg2;
    /* You must implement this function */
}

void iplc_sim_process_pipeline_jump(char *instruction) //TYLER
{
	iplc_sim_push_pipeline_stage(); //Step 1: push stage

	pipeline[FETCH].itype = JUMP;		//Step 2: set itype and address
	pipeline[FETCH].instruction_address = instruction_address;

	strcpy(pipeline[FETCH].stage.jump.instruction, instruction); //step 3: Copy specific variables/arguments
    /* You must implement this function */
}

void iplc_sim_process_pipeline_syscall() //TYLER
{
	iplc_sim_push_pipeline_stage(); //Step 1: push stage

	pipeline[FETCH].itype = SYSCALL;//Step 2: set itype and address
	pipeline[FETCH].instruction_address = instruction_address;
    /* You must implement this function */
}

void iplc_sim_process_pipeline_nop() //TYLER
{
	iplc_sim_push_pipeline_stage(); //Step 1: push stage

	pipeline[FETCH].itype = NOP;	//Step 2: set itype and address
	pipeline[FETCH].instruction_address = instruction_address;
    /* You must implement this function */
}

/************************************************************************************************/
/* parse Function *******************************************************************************/
/************************************************************************************************/

/*
 * Don't touch this function.  It is for parsing the instruction stream.
 */
unsigned int iplc_sim_parse_reg(char *reg_str)
{
    int i;
    // turn comma into \n
    if (reg_str[strlen(reg_str)-1] == ',')
        reg_str[strlen(reg_str)-1] = '\n';

    if (reg_str[0] != '$')
        return atoi(reg_str);
    else {
        // copy down over $ character than return atoi
        for (i = 0; i < strlen(reg_str); i++)
            reg_str[i] = reg_str[i+1];

        return atoi(reg_str);
    }
}

/*
 * Don't touch this function.  It is for parsing the instruction stream.
 */
void iplc_sim_parse_instruction(char *buffer)
{
    int instruction_hit = 0;
    int i=0, j=0;
    int src_reg=0;
    int src_reg2=0;
    int dest_reg=0;
    char str_src_reg[16];
    char str_src_reg2[16];
    char str_dest_reg[16];
    char str_constant[16];

    if (sscanf(buffer, "%x %s", &instruction_address, instruction ) != 2) {
        printf("Malformed instruction \n");
        exit(-1);
    }

    instruction_hit = iplc_sim_trap_address( instruction_address );

    // if a MISS, then push current instruction thru pipeline
    if (!instruction_hit) {
        // need to subtract 1, since the stage is pushed once more for actual instruction processing
        // also need to allow for a branch miss prediction during the fetch cache miss time -- by
        // counting cycles this allows for these cycles to overlap and not doubly count.

        printf("INST MISS:\t Address 0x%x \n", instruction_address);

        for (i = pipeline_cycles, j = pipeline_cycles; i < j + CACHE_MISS_DELAY - 1; i++)
            iplc_sim_push_pipeline_stage();
    }
    else
        printf("INST HIT:\t Address 0x%x \n", instruction_address);

    // Parse the Instruction

    if (strncmp( instruction, "add", 3 ) == 0 ||
        strncmp( instruction, "sll", 3 ) == 0 ||
        strncmp( instruction, "ori", 3 ) == 0) {
        if (sscanf(buffer, "%x %s %s %s %s",
                   &instruction_address,
                   instruction,
                   str_dest_reg,
                   str_src_reg,
                   str_src_reg2 ) != 5) {
            printf("Malformed RTYPE instruction (%s) at address 0x%x \n",
                   instruction, instruction_address);
            exit(-1);
        }

        dest_reg = iplc_sim_parse_reg(str_dest_reg);
        src_reg = iplc_sim_parse_reg(str_src_reg);
        src_reg2 = iplc_sim_parse_reg(str_src_reg2);

        iplc_sim_process_pipeline_rtype(instruction, dest_reg, src_reg, src_reg2);
    }

    else if (strncmp( instruction, "lui", 3 ) == 0) {
        if (sscanf(buffer, "%x %s %s %s",
                   &instruction_address,
                   instruction,
                   str_dest_reg,
                   str_constant ) != 4 ) {
            printf("Malformed RTYPE instruction (%s) at address 0x%x \n",
                   instruction, instruction_address );
            exit(-1);
        }

        dest_reg = iplc_sim_parse_reg(str_dest_reg);
        src_reg = -1;
        src_reg2 = -1;
        iplc_sim_process_pipeline_rtype(instruction, dest_reg, src_reg, src_reg2);
    }

    else if (strncmp( instruction, "lw", 2 ) == 0 ||
             strncmp( instruction, "sw", 2 ) == 0  ) {
        if ( sscanf( buffer, "%x %s %s %s %x",
                    &instruction_address,
                    instruction,
                    reg1,
                    offsetwithreg,
                    &data_address ) != 5) {
            printf("Bad instruction: %s at address %x \n", instruction, instruction_address);
            exit(-1);
        }

        if (strncmp(instruction, "lw", 2 ) == 0) {

            dest_reg = iplc_sim_parse_reg(reg1);

            // don't need to worry about base regs -- just insert -1 values
            iplc_sim_process_pipeline_lw(dest_reg, -1, data_address);
        }
        if (strncmp( instruction, "sw", 2 ) == 0) {
            src_reg = iplc_sim_parse_reg(reg1);

            // don't need to worry about base regs -- just insert -1 values
            iplc_sim_process_pipeline_sw( src_reg, -1, data_address);
        }
    }
    else if (strncmp( instruction, "beq", 3 ) == 0) {
        // don't need to worry about getting regs -- just insert -1 values
        iplc_sim_process_pipeline_branch(-1, -1);
    }
    else if (strncmp( instruction, "jal", 3 ) == 0 ||
             strncmp( instruction, "jr", 2 ) == 0 ||
             strncmp( instruction, "j", 1 ) == 0 ) {
        iplc_sim_process_pipeline_jump( instruction );
    }
    else if (strncmp( instruction, "jal", 3 ) == 0 ||
             strncmp( instruction, "jr", 2 ) == 0 ||
             strncmp( instruction, "j", 1 ) == 0 ) {
        /*
         * Note: no need to worry about forwarding on the jump register
         * we'll let that one go.
         */
        iplc_sim_process_pipeline_jump(instruction);
    }
    else if ( strncmp( instruction, "syscall", 7 ) == 0) {
        iplc_sim_process_pipeline_syscall( );
    }
    else if ( strncmp( instruction, "nop", 3 ) == 0) {
        iplc_sim_process_pipeline_nop( );
    }
    else {
        printf("Do not know how to process instruction: %s at address %x \n",
               instruction, instruction_address );
        exit(-1);
    }
}

/************************************************************************************************/
/* MAIN Function ********************************************************************************/
/************************************************************************************************/

int main()
{
    char trace_file_name[1024];
    FILE *trace_file = NULL;
    char buffer[80];
    int index = 10;
    int blocksize = 1;
    int assoc = 1;

    printf("Please enter the tracefile: ");
    scanf("%s", trace_file_name);

    trace_file = fopen(trace_file_name, "r");

    if ( trace_file == NULL ) {
        printf("fopen failed for %s file\n", trace_file_name);
        exit(-1);
    }

    printf("Enter Cache Size (index), Blocksize and Level of Assoc \n");
    scanf( "%d %d %d", &index, &blocksize, &assoc );

    printf("Enter Branch Prediction: 0 (NOT taken), 1 (TAKEN): ");
    scanf("%d", &branch_predict_taken );

    iplc_sim_init(index, blocksize, assoc);

    while (fgets(buffer, 80, trace_file) != NULL) {
        iplc_sim_parse_instruction(buffer);
        if (dump_pipeline)
            iplc_sim_dump_pipeline();
    }

    iplc_sim_finalize();
    return 0;
}
