# Robot-family-neutral, versioned NRT admission publication.
#
# The publisher and consumer use CLOCK_MONOTONIC/steady-clock timestamps on
# one host. The consumer stamps receipt locally and applies the smaller of
# lease_duration_ns and its configured lease cap.

string SCHEMA=manyforge.robot_lifecycle.execution_admission.v0.experimental

string schema
string provider_instance_id
string robot_id
string binding_contract_generation_id
string runtime_generation_id
string provider_component_epoch
string transport_owner_component_epoch
string authority_component_epoch
string cell_supervisor_component_epoch

uint64 publication_generation
uint64 lifecycle_generation
uint64 source_snapshot_generation

int64 projected_at_steady_time_ns
int64 lease_duration_ns

bool controller_ready
bool motion_admitted
string[] reason_codes
