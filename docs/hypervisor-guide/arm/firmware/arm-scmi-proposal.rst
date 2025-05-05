
Proposal for SCMI multi-agent driver bindings
=============================================

Now the Xen configuration for SCMI multi-agent support is done in a bit complicated way, especially
from SCMI multi-agent driver initialization and Dom0 DT manipulation point of view.
Also it does not take into account future requirements to support SCP SCMI FW.

To enable SCMI multi-agent user need:

* take host DT with basic SCMI enabled
* add SCMI shared-memory nodes for all agents
* update SCMI node to point on SCMI Xen management channel (``[smc-id, shmem]``)
* add "xen,scmi-secondary-agents" property to the "\chosen" node

.. code::

   chosen {
      xen,scmi-secondary-agents = <
                    1 0x82000003 &scmi_shm_1
                    2 0x82000004 &scmi_shm_2
                    3 0x82000005 &scmi_shm_3
                    4 0x82000006 &scmi_shm_4>;
    }

    /{
            // SCMI shared-memory nodes for all agents
            scmi_shm_0 : sram@47ff0000 {
                compatible = "arm,scmi-shmem";
                reg = <0x0 0x47ff0000 0x0 0x1000>;
            };
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

            firmware {
                scmi: scmi {
                    compatible = "arm,scmi-smc";
                    arm, smc - id = <0x82000002>; <--- Xen management agent channel "smc-id"
                    #address-cells = < 1>;
                    #size-cells = < 0>;
                    #access-controller-cells = < 1>;
                    shmem = <&scmi_shm_0>; <--- Xen management agent channel "shmem"

                    protocol@X{
                    };
                };
            };
    }

Important thing to note is that all information about multi-channel support is strictly Xen specific.

During initialization the SCMI multi-agent driver uses Host DT SCMI node and
"xen,scmi-secondary-agents" property to init itself and then, during Dom0 creation, manipulates
Dom0 DT to remove Xen specific SCMI info and update dom0 SCMI nodes with Dom0 SCMI agent specific
information.

There are two negative points:

1) Double DT modification - one is user to set up SCMI Xen support in Host DT, second -
   Dom0 DT manipulation.
2) In case of future support of mailbox shared-memory transport there could be up to 4 mailboxes and
   up to 2 shared-memories per SCMI agent channel.

Hence SCMI multi-agent support is Xen specific knowledge there is a proposal to add it as Xen
specific DT definitions and so minimize Host and Dom0 DT manipulations.
Those definitions can be added in "/chosen" or, ideally, in "xen,config" node (like in Hyperlaunch design).

The SCMI binding stays generic, just two SCMI nodes defined - one for Xen management channel and
one for Host Dom0 OSPM.

Example of using "chosen" for configuration:

.. code::

    /{

        chosen {
            ...

            // Xen SCMI management channel
            scmi_shm_0 : sram@47ff0000 {
                compatible = "arm,scmi-shmem";
                reg = <0x0 0x47ff0000 0x0 0x1000>;
            };
            scmi_xen: scmi {
                compatible = "arm,scmi-smc";
                arm,smc-id = <0x82000002>; <--- Xen manegement agent smc-id
                #address-cells = < 1>;
                #size-cells = < 0>;
                #access-controller-cells = < 1>;
                shmem = <&scmi_shm_0>; <--- Xen manegement agent shmem
            };

            // SCMI multi-agent configuration
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
            xen,scmi-secondary-agents = <
                        1 0x82000003 &scmi_shm
                        2 0x82000004 &scmi_shm_2
                        3 0x82000005 &scmi_shm_3
                        4 0x82000006 &scmi_shm_4>;
        };

        // Host SCMI OSPM channel - provided to the Dom0 as is if SCMI enabled for it
        scmi_shm: sram@47ff1000 {
                compatible = "arm,scmi-shmem";
                reg = <0x0 0x47ff1000 0x0 0x1000>;
        };

        firmware {
            scmi: scmi {
                compatible = "arm,scmi-smc";
                arm,smc-id = <0x82000003>; <--- Host OSPM agent smc-id
                #address-cells = < 1>;
                #size-cells = < 0>;
                shmem = <&scmi_shm>; <--- Host OSPM agent shmem

                protocol@X{
                };
            };
        };
    }


In the above case:

1) Xen SCMI multi-agent can be probed with DT configuration from "chosen" (or special "xen,config")
   node and all Xen related nodes can be easily dropped from Dom0 DT.
2) Host SCMI OSPM channel DT nodes can be copied to Dom0 DT without changes if SCMI enabled for it.
3) Future support for mailbox shared-memory transport (SCP SCMI FW) can be simplified as no more
   manipulation required with Dom0 SCMI "arm,smc-id" and "shmem" DT properties.


Example of using "xen,config" for configuration:

.. code::

    hypervisor {
        compatible = “hypervisor,xen”

        // Configuration container
        config {
            compatible = "xen,config";
            ...

            // Xen SCMI management channel
            scmi_shm_0 : sram@47ff0000 {
                compatible = "arm,scmi-shmem";
                reg = <0x0 0x47ff0000 0x0 0x1000>;
            };
            scmi_xen: scmi {
                compatible = "arm,scmi-smc";
                arm,smc-id = <0x82000002>; <--- Xen manegement agent smc-id
                #address-cells = < 1>;
                #size-cells = < 0>;
                #access-controller-cells = < 1>;
                shmem = <&scmi_shm_0>; <--- Xen manegement agent shmem
            };

            // SCMI multi-agent configuration
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
            xen,scmi-secondary-agents = <
                        1 0x82000003 &scmi_shm
                        2 0x82000004 &scmi_shm_2
                        3 0x82000005 &scmi_shm_3
                        4 0x82000006 &scmi_shm_4>;
        };
    };

    /{
        // Host SCMI OSPM channel - provided to the Dom0 as is if SCMI enabled for it
        scmi_shm: sram@47ff1000 {
                compatible = "arm,scmi-shmem";
                reg = <0x0 0x47ff1000 0x0 0x1000>;
        };

        firmware {
            scmi: scmi {
                compatible = "arm,scmi-smc";
                arm,smc-id = <0x82000003>; <--- Host OSPM agent smc-id
                #address-cells = < 1>;
                #size-cells = < 0>;
                shmem = <&scmi_shm>; <--- Host OSPM agent shmem

                protocol@X{
                };
            };
        };
    }
