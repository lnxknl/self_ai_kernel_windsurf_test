#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "topology.h"

/* Global variables */
static struct cpu_topology cpu_topology[MAX_CPUS];
static unsigned long cpu_capacity[MAX_CPUS];
static unsigned long max_capacity = 0;

/* CPU efficiency table */
static const struct cpu_efficiency table_efficiency[] = {
    {"arm,cortex-a15", 3891},
    {"arm,cortex-a7",  2048},
    {"arm,cortex-a53", 2048},
    {"arm,cortex-a72", 4096},
    {NULL, 0},
};

void init_cpu_topology(void) {
    memset(cpu_topology, 0, sizeof(cpu_topology));
    memset(cpu_capacity, 0, sizeof(cpu_capacity));
    
    for (int i = 0; i < MAX_CPUS; i++) {
        cpu_topology[i].present = false;
        cpu_capacity[i] = SCHED_CAPACITY_SCALE;
    }
}

static unsigned long get_cpu_efficiency(const char *cpu_type) {
    const struct cpu_efficiency *table = table_efficiency;
    
    while (table->compatible) {
        if (strcmp(table->compatible, cpu_type) == 0)
            return table->efficiency;
        table++;
    }
    
    return SCHED_CAPACITY_SCALE;
}

int add_cpu(unsigned int cpuid, const char *cpu_type, 
            unsigned int thread_id, unsigned int core_id, 
            unsigned int cluster_id) {
    if (cpuid >= MAX_CPUS)
        return -1;
        
    cpu_topology[cpuid].thread_id = thread_id;
    cpu_topology[cpuid].core_id = core_id;
    cpu_topology[cpuid].cluster_id = cluster_id;
    cpu_topology[cpuid].present = true;
    
    /* Calculate CPU capacity based on efficiency */
    unsigned long efficiency = get_cpu_efficiency(cpu_type);
    cpu_capacity[cpuid] = (efficiency * SCHED_CAPACITY_SCALE) / 3891;
    
    if (cpu_capacity[cpuid] > max_capacity)
        max_capacity = cpu_capacity[cpuid];
        
    return 0;
}

int remove_cpu(unsigned int cpuid) {
    if (cpuid >= MAX_CPUS)
        return -1;
        
    cpu_topology[cpuid].present = false;
    cpu_capacity[cpuid] = 0;
    
    /* Recalculate max capacity */
    max_capacity = 0;
    for (int i = 0; i < MAX_CPUS; i++) {
        if (cpu_topology[i].present && cpu_capacity[i] > max_capacity)
            max_capacity = cpu_capacity[i];
    }
    
    return 0;
}

void update_cpu_capacity(unsigned int cpu) {
    if (cpu >= MAX_CPUS || !cpu_topology[cpu].present)
        return;
        
    /* Normalize capacity to max capacity */
    if (max_capacity > 0) {
        cpu_capacity[cpu] = (cpu_capacity[cpu] * SCHED_CAPACITY_SCALE) / max_capacity;
    }
}

unsigned long get_cpu_capacity(unsigned int cpu) {
    if (cpu >= MAX_CPUS)
        return 0;
    return cpu_capacity[cpu];
}

void print_cpu_topology(void) {
    printf("\nCPU Topology Information:\n");
    printf("CPU\tPresent\tThread\tCore\tCluster\tCapacity\n");
    printf("----------------------------------------\n");
    
    for (int i = 0; i < MAX_CPUS; i++) {
        if (cpu_topology[i].present) {
            printf("%d\tYes\t%d\t%d\t%d\t%lu\n",
                   i,
                   cpu_topology[i].thread_id,
                   cpu_topology[i].core_id,
                   cpu_topology[i].cluster_id,
                   cpu_capacity[i]);
        }
    }
}

/* Example usage */
int main() {
    /* Initialize topology subsystem */
    init_cpu_topology();
    
    /* Add some CPUs */
    add_cpu(0, "arm,cortex-a72", 0, 0, 0);  /* Big core */
    add_cpu(1, "arm,cortex-a72", 1, 0, 0);  /* Big core */
    add_cpu(2, "arm,cortex-a53", 0, 1, 1);  /* LITTLE core */
    add_cpu(3, "arm,cortex-a53", 1, 1, 1);  /* LITTLE core */
    
    /* Update capacities */
    for (int i = 0; i < MAX_CPUS; i++) {
        update_cpu_capacity(i);
    }
    
    /* Print topology */
    print_cpu_topology();
    
    /* Remove a CPU */
    printf("\nRemoving CPU 1...\n");
    remove_cpu(1);
    
    /* Update capacities again */
    for (int i = 0; i < MAX_CPUS; i++) {
        update_cpu_capacity(i);
    }
    
    /* Print final topology */
    print_cpu_topology();
    
    return 0;
}
