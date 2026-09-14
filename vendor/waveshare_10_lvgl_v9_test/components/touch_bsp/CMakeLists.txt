idf_component_register(
  SRCS "touch_bsp.c"
  PRIV_REQUIRES i2c_bsp main
  INCLUDE_DIRS "./")
