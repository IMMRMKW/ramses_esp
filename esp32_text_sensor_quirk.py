"""Quirk for ESP32-C6 Zigbee Text Sensor."""

from zigpy.quirks import CustomCluster, CustomDevice
from zigpy.zcl.clusters.general import Basic, Identify
from zigpy import types
from zigpy.zcl import foundation

from zhaquirks.const import (
    DEVICE_TYPE,
    ENDPOINTS,
    INPUT_CLUSTERS,
    MODELS_INFO,
    OUTPUT_CLUSTERS,
    PROFILE_ID,
)


class CustomTextSendCluster(CustomCluster):
    """Custom cluster for sending text (ESP32 -> ZHA via client commands)."""

    cluster_id = 0xFC00
    
    class ClientCommandDefs(foundation.BaseCommandDefs):
        """Commands that ESP32 (client) sends to coordinator (server)."""
        
        send_text: foundation.ZCLCommandDef = foundation.ZCLCommandDef(
            id=0x00,
            schema={"text": types.CharacterString},
        )


class CustomTextReceiveCluster(CustomCluster):
    """Custom cluster for receiving text (ZHA -> ESP32)."""

    cluster_id = 0xFC01
    
    class AttributeDefs(foundation.BaseAttributeDefs):
        """Attribute definitions."""
        text_string: foundation.ZCLAttributeDef = foundation.ZCLAttributeDef(
            id=0x0000,
            type=types.CharacterString,
            access="rwp",  # Read, write, and reportable (matches ESP32 READ_WRITE)
            is_manufacturer_specific=False,
        )

    class ClientCommandDefs(foundation.BaseCommandDefs):
        """Commands the client (ZHA) can send to this server."""

        set_text: foundation.ZCLCommandDef = foundation.ZCLCommandDef(
            id=0x00,
            schema={"text": types.CharacterString},
        )
        
        set_text_heartbeat: foundation.ZCLCommandDef = foundation.ZCLCommandDef(
            id=0x01,
            schema={"text": types.CharacterString},
        )

    class ServerCommandDefs(foundation.BaseCommandDefs):
        """Commands this server sends back (ESP32 -> ZHA)."""

        set_text_response: foundation.ZCLCommandDef = foundation.ZCLCommandDef(
            id=0x01,
            schema={"text": types.CharacterString},
        )


class ESP32TextSensor(CustomDevice):
    """ESP32-C6 Zigbee Text Sensor."""

    signature = {
        MODELS_INFO: [
            ("ELECRAM", "Ramses_esp32c6"),
        ],
        ENDPOINTS: {
            10: {
                PROFILE_ID: 0x0104,  # Home Automation
                INPUT_CLUSTERS: [
                    Basic.cluster_id,  # 0x0000
                    Identify.cluster_id,  # 0x0003
                    0xFC01,  # Custom Text Receive Cluster (server - receives commands from ZHA)
                ],
                OUTPUT_CLUSTERS: [
                    0xFC00,  # Custom Text Send Cluster (client - sends commands to ZHA)
                ],
            }
        },
    }

    replacement = {
        ENDPOINTS: {
            10: {
                PROFILE_ID: 0x0104,
                DEVICE_TYPE: 0x0302,
                INPUT_CLUSTERS: [
                    Basic.cluster_id,
                    Identify.cluster_id,
                    CustomTextReceiveCluster,
                ],
                OUTPUT_CLUSTERS: [
                    CustomTextSendCluster,
                ],
            }
        },
    }
