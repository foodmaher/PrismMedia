# Prism rear-camera accessory mod source

This folder is the cosmetic/shop half of the Prism rear-camera kit. The plugin
provides the live render and GPS connection; this mod provides the truck-shop
accessory.

SCS truck accessories are truck-specific and attach through model locators, so
the supplied definitions target the 2016 Scania R/S accessory layout used for
the first test. Export `prism_camera.pim` with SCS Blender Tools and Conversion
Tools, then place the generated PMD/PMG files at the model path referenced by
the definitions.

The plugin cannot read the player's purchased accessory list through the
public telemetry API. After buying the accessory, enable **Camera kit installed
on this vehicle** once in the plugin menu; that setting is saved per screen.
