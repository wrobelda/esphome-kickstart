import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import web_server_base
from esphome.components.web_server_base import CONF_WEB_SERVER_BASE_ID
from esphome.const import CONF_ID, CONF_OFFSET, CONF_SIZE

DEPENDENCIES = ["esp8266", "web_server"]
AUTO_LOAD = ["web_server_base"]

CONF_FLASH_SIZE = "flash_size"
CONF_SLOTS = "slots"

slot_control_ns = cg.esphome_ns.namespace("kickstart_slot_control")
KickstartSlotControl = slot_control_ns.class_("KickstartSlotControl", cg.Component)

SLOT_SCHEMA = cv.Schema(
    {cv.Required(CONF_OFFSET): cv.positive_int, cv.Required(CONF_SIZE): cv.positive_int}
)


def _validate_layout(config):
    slots = sorted(config[CONF_SLOTS], key=lambda slot: slot[CONF_OFFSET])
    if len(slots) != 2:
        raise cv.Invalid("slot control requires exactly two slots")
    previous_end = 0
    for slot in slots:
        offset = slot[CONF_OFFSET]
        end = offset + slot[CONF_SIZE]
        if offset < previous_end:
            raise cv.Invalid("slot ranges overlap")
        if end > config[CONF_FLASH_SIZE]:
            raise cv.Invalid("slot range exceeds flash_size")
        previous_end = end
    config[CONF_SLOTS] = slots
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(KickstartSlotControl),
            cv.GenerateID(CONF_WEB_SERVER_BASE_ID): cv.use_id(web_server_base.WebServerBase),
            cv.Required(CONF_FLASH_SIZE): cv.positive_int,
            cv.Required(CONF_SLOTS): cv.ensure_list(SLOT_SCHEMA),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _validate_layout,
)


async def to_code(config):
    server = await cg.get_variable(config[CONF_WEB_SERVER_BASE_ID])
    var = cg.new_Pvariable(config[CONF_ID], server)
    await cg.register_component(var, config)
    cg.add(var.set_flash_size(config[CONF_FLASH_SIZE]))
    for index, slot in enumerate(config[CONF_SLOTS]):
        cg.add(var.set_slot(index, slot[CONF_OFFSET], slot[CONF_SIZE]))
