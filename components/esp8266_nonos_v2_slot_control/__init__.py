import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import web_server_base
from esphome.components.esp8266_nonos_v2_to_eboot_v1 import Esp8266NonosV2ToEbootV1
from esphome.components.web_server_base import CONF_WEB_SERVER_BASE_ID
from esphome.const import CONF_ID

DEPENDENCIES = ["esp8266", "web_server"]
AUTO_LOAD = ["web_server_base"]

CONF_MIGRATION_ID = "migration_id"
CONF_AUTO_COPY_LOWER_TO_UPPER_SLOT = "auto_copy_lower_to_upper_slot"

slot_control_ns = cg.esphome_ns.namespace("esp8266_nonos_v2_slot_control")
Esp8266NonosV2SlotControl = slot_control_ns.class_(
    "Esp8266NonosV2SlotControl", cg.Component
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(Esp8266NonosV2SlotControl),
        cv.GenerateID(CONF_WEB_SERVER_BASE_ID): cv.use_id(
            web_server_base.WebServerBase
        ),
        cv.GenerateID(CONF_MIGRATION_ID): cv.use_id(Esp8266NonosV2ToEbootV1),
        cv.Optional(CONF_AUTO_COPY_LOWER_TO_UPPER_SLOT, default=False): cv.boolean,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    server = await cg.get_variable(config[CONF_WEB_SERVER_BASE_ID])
    migration = await cg.get_variable(config[CONF_MIGRATION_ID])
    var = cg.new_Pvariable(config[CONF_ID], server, migration)
    await cg.register_component(var, config)
    cg.add(
        var.set_auto_copy_lower_to_upper_slot(
            config[CONF_AUTO_COPY_LOWER_TO_UPPER_SLOT]
        )
    )
