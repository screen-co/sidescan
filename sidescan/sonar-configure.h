#ifndef __SENSORS_H__
#define __SENSORS_H__

#include <hyscan-sonar-control.h>

/* Функция настраивает местоположение антенн датчиков и активирует приём данных. */
gboolean       setup_sensors           (HyScanSensorControl           *control,
                                        GKeyFile                      *config);

/* Функция настраивает местоположение антенн гидролокатора. */
gboolean       setup_sonar_antenna     (HyScanSonarControl            *control,
                                        HyScanSourceType               source,
                                        GKeyFile                      *config);

#endif /* __SENSORS_H__ */
