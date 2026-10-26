"""Shared UPS data types used by ups_hid and nut_server."""

import esphome.codegen as cg

CODEOWNERS = ["@jamesgoodhouse"]

ups_common_ns = cg.esphome_ns.namespace("ups_common")
UpsSource = ups_common_ns.class_("UpsSource")
