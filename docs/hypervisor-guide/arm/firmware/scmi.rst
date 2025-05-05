.. SPDX-License-Identifier: CC-BY-4.0

ARM System Control and Management Interface (SCMI)
==================================================

The System Control and Management Interface (SCMI) [1], which is a set of operating
system-independent software interfaces that are used in system management. SCMI currently
provides interfaces for:

- Discovery and self-description of the interfaces it supports
- Power domain management
- Clock management
- Reset domain management
- Voltage domain management
- Sensor management
- Performance management
- Power capping and monitoring
- Pin control protocol.

The SCMI compliant firmware could run:

- as part of EL3 secure world software (like Trusted Firmware-A) with
  ARM SMC/HVC shared-memory transport;
- on dedicated System Control Processor (SCP) with HW mailbox shared-memory transport

The major purpose of enabling SCMI support in Xen is to enable guest domains access to the SCMI
interfaces for performing management actions on passed-through devices (such as clocks/resets etc)
without accessing directly to the System control HW (like clock controllers) which in most cases
can't shared/split between domains. Or, at minimum, allow SCMI access for dom0/hwdom (or guest
domain serving as Driver domain).

The below sections describe SCMI support options available for Xen.

| [1] `Arm SCMI <https://developer.arm.com/documentation/den0056/latest/>`_
| [2] `System Control and Management Interface (SCMI) bindings <https://web.git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/Documentation/devicetree/bindings/firmware/arm,scmi.yaml>`_
| [3] `Generic Domain Access Controllers bindings <https://web.git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/Documentation/devicetree/bindings/access-controllers/access-controllers.yaml>`_


Simple SCMI over SMC/HVC calls forwarding driver (EL3)
------------------------------------------------------

The EL3 SCMI firmware (TF-A) with a single SCMI OSPM agent support is pretty generic case for
the default vendors SDK and new platforms with SCMI support. Such EL3 SCMI firmware supports only
single SCMI OSPM transport (agent) with Shared memory based transport and SMC/HVC calls as doorbell.

The SCMI over SMC/HVC calls forwarding driver solves major problem for this case by allowing
SMC/HVC calls to be forwarded form guest to the EL3 SCMI firmware.

By default, the SCMI over SMC/HVC calls forwarding is enabled for Dom0/hwdom.

::

    +--------------------------+
    |                          |
    | EL3 SCMI FW (TF-A)       |
    ++-------+--^--------------+
     |shmem  |  | smc-id
     +----^--+  |
          |     |
     +----|-+---+---+----------+
     |    | |  FWD  |      Xen |
     |    | +---^---+          |
     +----|-----|--------------+
          |     | smc-id
     +----v-----+--+ +---------+
     |             | |         |
     | Dom0/hwdom  | | DomU    |
     |             | |         |
     |             | |         |
     +-------------+ +---------+


The SCMI messages are passed directly through SCMI shared-memory (zero-copy) and driver only
forwards SMC/HVC calls.

Compiling
^^^^^^^^^

To build with the SCMI over SMC/HVC calls forwarding enabled support, enable Kconfig option

::

    CONFIG_SCMI_SMC

The ``CONFIG_SCMI_SMC`` is enabled by default.

Pass-through SCMI SMC to domain which serves as Driver domain
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

This section describes how to configure the SCMI over SMC/HVC calls forwarding driver to handle use
case "thin Dom0 with guest domain, which serves as Driver domain". In this case HW need to be
enabled in Driver domain and dom0 is performing only control functions (without accessing FW) and so,
the SCMI need to be enabled in Driver domain.

::

     +--------------------------+
     |EL3 SCMI FW (TF-A)        |
     |                          |
     +-------------^--+-------+-+
             smc-id|  |shmem0 |
                   |  +----^--+
    +-------------++------+|----+
    |Xen          |  FWD  ||    |
    |             +--^----+|    |
    +----------------|-----|----+
              smc-id |     |
    +-----------+ +--+-----v-----+
    |           | |              |
    | Dom0      | |    Driver    |
    | Control   | |    domain    |
    |           | |              |
    +-----------+ +--------------+

The SCMI can be enabled for one and only one guest domain.

First. configure Dom0 to enable SCMI pass-through using Xen Command Line
**"dom0_scmi_smc_passthrough"** option. This will disable SCMI for Dom0/hwdom and SCMI nodes will
be removed from Dom0/hwdom device tree.

**Configure SCMI pass-through for guest domain with toolstack**

* In domain's xl.cfg file add **"arm_sci"** option as below

::

    arm_sci = "type=scmi_smc"

* In domain's xl.cfg file enable access to the "arm,scmi-shmem"

::

    iomem = [
        "47ff0,1@22001",
    ]

.. note:: It's up to the user to select guest IPA for mapping SCMI shared-memory.

* Add SCMI nodes to the Driver domain partial device tree as in the below example:

.. code::

    passthrough {
       scmi_shm_0: sram@22001000 {
           compatible = "arm,scmi-shmem";
           reg = <0x0 0x22001000 0x0 0x1000>;
       };

       firmware {
            compatible = "simple-bus";
                scmi: scmi {
                    compatible = "arm,scmi-smc";
                    shmem = <&scmi_shm_0>;
                    ...
                }
        }
    }

In general, the configuration is similar to any other HW pass-through, except explicitly
enabling SCMI with "arm_sci" xl.cfg option.

**Configure SCMI pass-through for predefined domain (dom0less)**

* add "xen,sci_type" property for required DomU ("xen,domain") node

::

       xen,sci_type="scmi_smc"

* add scmi nodes to the Driver domain partial device tree the same way as above and enable access
  to the "arm,scmi-shmem" according to  dom0less documentation. For example:

.. code::

      scmi_shm_0: sram@22001000 {
            compatible = "arm,scmi-shmem";
            reg = <0x00 0x22001000 0x00 0x1000>;
    ->        xen,reg = <0x0 0x47ff0000 0x0 0x1000 0x0 0x22001000>;
    ->        xen,force-assign-without-iommu;
      };

Simple SCMI SMC/HVC multi-agent driver (EL3)
------------------------------------------------

The SCMI SMC/HVC multi-agent driver enables support for ARM EL3 Trusted Firmware-A (TF-A) which
provides SCMI interface with multi-agnet support, as shown below.

::

      +-----------------------------------------+
      |                                         |
      | EL3 TF-A SCMI                           |
      +-------+--+-------+--+-------+--+-------++
      |shmem0 |  |shmem1 |  |shmem2 |  |shmemX |
      +-----+-+  +---+---+  +--+----+  +---+---+
    smc-id0 |        |         |           |
    agent0  |        |         |           |
      +-----v--------+---------+-----------+----+
      |              |         |           |    |
      |              |         |           |    |
      +--------------+---------+-----------+----+
             smc-id1 |  smc-id2|    smc-idX|
             agent1  |  agent2 |    agentX |
                     |         |           |
                +----v---+  +--v-----+  +--v-----+
                |        |  |        |  |        |
                | Dom0   |  | Dom1   |  | DomX   |
                |        |  |        |  |        |
                |        |  |        |  |        |
                +--------+  +--------+  +--------+

The EL3 SCMI multi-agent firmware expected to provide SCMI SMC/HVC shared-memory transport
for every Agent in the system. The SCMI Agent transport channel defined by pair:

- smc-id: SMC/HVC function id used for Doorbell
- shmem: shared memory for messages transfer, **Xen page aligned** with mapping``p2m_mmio_direct_nc``.

The following SCMI Agents expected to be defined by SCMI FW to enable SCMI multi-agent functionality
under Xen:

- Xen management agent: trusted agents that accesses to the Base Protocol commands to configure
  agent specific permissions
- OSPM VM agents: non-trusted agent, one for each Guest domain which is  allowed direct HW access.
  At least one OSPM VM agent has to be provided by FW if HW is handled only by Dom0 or Driver Domain.

The EL3 SCMI FW expected to implement following Base protocol messages:

- BASE_DISCOVER_AGENT
- BASE_RESET_AGENT_CONFIGURATION (optional)
- BASE_SET_DEVICE_PERMISSIONS (optional)

The number of supported SCMI agents and their transport specifications are SCMI FW implementation
specific.


Compiling
^^^^^^^^^

To build with the SCMI SMC/HVC multi-agent driver support, enable Kconfig option:

::

    CONFIG_SCMI_SMC_MA


Driver functionality
^^^^^^^^^^^^^^^^^^^^

The SCI SCMI SMC multi-agent driver implements following functionality:

- The driver is initialized based on the Host DT SCMI node (only one SCMI interface is supported)
  which describes Xen management agent SCMI interface.

.. code::

    scmi_shm_0 : sram@47ff0000 {
        compatible = "arm,scmi-shmem";
        reg = <0x0 0x47ff0000 0x0 0x1000>;
    };
    firmware {
        scmi: scmi {
            compatible = "arm,scmi-smc";
            arm, smc - id = <0x82000002>; <--- Xen manegement agent smc-id
            \#address-cells = < 1>;
            \#size-cells = < 0>;
            \#access-controller - cells = < 1>;
            shmem = <&scmi_shm_0>; <--- Xen manegement agent shmem

            protocol@X{
            };
        };
    };

- The driver obtains Xen specific SCMI Agent's configuration from the Host DT, probes Agents and
  builds SCMI Agents list. The Agents configuration is taken from "xen,scmi-secondary-agents"
  property where first item is SCMI "agent_id", second - "arm,smc-id", and
  third - "arm,scmi-shmem" phandle for this "agent_id":

.. code::

    chosen {
      xen,scmi-secondary-agents = <
                    1 0x82000003 &scmi_shm_1
                    2 0x82000004 &scmi_shm_2
                    3 0x82000005 &scmi_shm_3
                    4 0x82000006 &scmi_shm_4>;
    }

    /{
            scmi_shm_1: sram@47ff1000 {
                    compatible = "arm,scmi-shmem";
                    reg = <0x0 0x47ff1000 0x0 0x1000>;
            };
            scmi_shm_2: sram@47ff2000 {
                    compatible = "arm,scmi-shmem";
                    reg = <0x0 0x47ff2000 0x0 0x1000>;
            };
            scmi_shm_3: sram@47ff3000 {
                    compatible = "arm,scmi-shmem";
                    reg = <0x0 0x47ff3000 0x0 0x1000>;
            };
            scmi_shm_4: sram@47ff4000 {
                    compatible = "arm,scmi-shmem";
                    reg = <0x0 0x47ff4000 0x0 0x1000>;
            };
    }


.. note::

    Note that Xen is the only one entry in the system which need to know about SCMI multi-agent support.

- The driver implements the SCI subsystem interface required for configuring and enabling SCMI
  functionality for Dom0/hwdom and Guest domains. To enable SCMI functionality for guest domain
  it has to be configured with unique supported SCMI Agent_id and use corresponding SCMI SMC/HVC
  shared-memory transport ``[smc-id, shmem]`` defined for this SCMI Agent_id.

- Once Xen domain is configured it can communicate with EL3 SCMI FW:

  - zero-copy, the guest domain puts/gets SCMI message in/from shmem;
  - the guest triggers SMC/HVC exception with agent "smc-id" (doorbell);
  - the Xen driver catches exception, do checks and synchronously forwards it to EL3 FW.

- the Xen driver sends BASE_RESET_AGENT_CONFIGURATION message to Xen management agent channel on
  domain destroy event. This allows to reset resources used by domain and so implement use-case
  like domain reboot.


Configure SCMI for Dom0
^^^^^^^^^^^^^^^^^^^^^^^

The **"dom0_scmi_agent_id=<dom0_agent_id>"** Xen command line is used to enable SCMI functionality for
Dom0. if not provided SCMI will be disabled for Dom0 and all SCMI nodes removed from Dom0 DT.

The driver updates Dom0 DT SCMI node "arm,smc-id" property with value and fixes up "shmem" property
according to the assigned for Dom0 SCMI "agent_id".

Configure SCMI for for guest domain with toolstack
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

* In domain's xl.cfg file add **"arm_sci"** option as below

::

    arm_sci = "type=scmi_smc_multiagent,agent_id=2"

* In domain's xl.cfg file enable access to the "arm,scmi-shmem" which should correspond
  assigned "agent_id" for the domain, for example:

::

    iomem = [
        "47ff2,1@22001",
    ]

.. note:: It's up to the user to select guest IPA for mapping SCMI shared-memory.

* Add SCMI nodes to the Driver domain partial device tree as in the below example.
  The "arm,smc-id" should correspond assigned agent_id for the domain:

.. code::

    passthrough {
       scmi_shm_0: sram@22001000 {
           compatible = "arm,scmi-shmem";
           reg = <0x0 0x22001000 0x0 0x1000>;
       };

       firmware {
            compatible = "simple-bus";
                scmi: scmi {
                    compatible = "arm,scmi-smc";
                    arm,smc-id = <0x82000004>;
                    shmem = <&scmi_shm_0>;
                    ...
                }
        }
    }

**Device specific access control**

The XEN SCMI SMC/HVC multi-agent driver performs "access-controller" provider function in case
EL3 SCMI FW implements SCMI "4.2.1.1 Device specific access control" and provides the
BASE_SET_DEVICE_PERMISSIONS command to configure the devices that an agents have access to.
The Host DT SCMI node should have "#access-controller-cells=<1>" property and DT devices should
be bound to the SCMI node using Access Controllers bindings [3].

For example:

.. code::

    &i2c1 {
            access-controllers = <&scmi 0>;
    };

Use domain's xl.cfg file **"dtdev"** property to assign SCMI devices from toolstack to the guest:

::

    dtdev = [
        "/soc/i2c@e6508000",
    ]

.. note::

    xl.cfg:"dtdev" need contain all nodes which are under SCMI management (not only those which are
    behind IOMMU) and passed-through to the guest domain.

Configure SCMI for predefined domains (dom0less)
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

* add "xen,sci_type" and "xen,sci_agent_id" properties for required DomU ("xen,domain") node

::

    xen,sci_type="scmi_smc_multiagent"
    xen,sci_agent_id=2

* add scmi nodes to the Driver domain partial device tree the same way as above (toolstack case) and
  enable access to the "arm,scmi-shmem" according to the dom0less documentation. For example:

.. code::

      scmi_shm_0: sram@22001000 {
            compatible = "arm,scmi-shmem";
            reg = <0x00 0x22001000 0x00 0x1000>;
    ->        xen,reg = <0x0 0x47ff0000 0x0 0x1000 0x0 0x22001000>;
    ->        xen,force-assign-without-iommu;
      };

* For SCMI device access control configure pass-through devices in the guest partial DT according to
  the dom0less documentation and ensure that devices SCMI management has "xen,path" property set:

.. code::

		i2c@e6508000 {
            ...
			reg = <0x00 0xe6508000 0x00 0x1000>;
    ->        xen,path = "/soc/i2c@e6508000"
    ->        xen,reg = <0x0 0xe6508000 0x0 0x1000 0x0 0xe6508000>;
    ->        xen,force-assign-without-iommu;
        };
