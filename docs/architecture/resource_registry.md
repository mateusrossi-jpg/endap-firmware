# Resource Registry Foundation

## Architecture Overview
The Resource Registry is the foundational abstraction layer for ENDAP. It abstracts hardware details (GPIO numbers, physical mappings) into operational resources (Temperature, Pump, Relay, Level). This ensures that operations, alarms, events, and automations operate on abstract entities, independent of physical topology changes.

## Layers
1. **Capability**: Platform support (e.g., node can do digital IO, read 1-wire, read I2C).
2. **Binding**: The mapping of a capability to a physical port (e.g., DS18B20 on GPIO33).
3. **Resource**: The logical representation (e.g., `water_temperature_sensor`).
4. **Operations/Alarms/Events**: Interact directly with the Resource, oblivious to the Binding.

## Data Model (Phase 0 - Read Only)
The initial model aggregates the current configuration from:
- Cluster Nodes
- Device Profiles (Inputs, Outputs, ADC, etc.)
- IO Maps
- Sensor Registry
- Installation Map

Each resource provides:
- `resource_id`: A unique logical identifier (e.g., derived from local_code or alias).
- `name`: Human-readable name.
- `type`: Resource type (e.g., `digital_input`, `digital_output`, `temperature_sensor`, `humidity_sensor`, `analog_input`).
- `node_name`: The name of the node where the resource resides.
- `binding`: Describes the physical mapping (e.g., `{"type": "gpio", "gpio": 32}`).
- `state`: Current state value (e.g., `ON`, `OFF`, `24.5`).
- `health`: The operational status of the binding (`Healthy`, `Faulty`, etc.).

## REST API
**`GET /api/resources`**
Returns a JSON aggregation of all clustered resources. It traverses local resources and node registry information to provide a unified catalog for the dashboard.

## Future Phases
- Phase 1: Allow runtime rebinding of resources.
- Phase 2: Migrate Distributed Rules Engine, Event Book, and Alarm Manager to operate solely on `resource_id`.
- Phase 3: Add support for external fieldbus sensors (Modbus/MQTT) as first-class resources.
