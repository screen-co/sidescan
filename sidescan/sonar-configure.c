#include "sonar-configure.h"

gboolean
setup_sensors (HyScanSensorControl *control,
               GKeyFile            *config)
{
  gboolean status = FALSE;
  gchar **ports = NULL;
  guint i, j;

  if (config == NULL)
    return TRUE;

  /* Настраиваем порты для подключения датчиков. */
  ports = hyscan_sensor_control_list_ports (control);
  if (ports != NULL)
    {
      for (i = 0; ports[i] != NULL; i++)
        {
          HyScanSensorPortType port_type;
          gint64 time_offset;
          guint channel;

          HyScanDataSchemaEnumValue **uart_devices;
          HyScanDataSchemaEnumValue **uart_modes;
          HyScanDataSchemaEnumValue **ip_addresses;

          HyScanAntennaPosition position;

          /* Порт не используется. */
          if (!g_key_file_has_group (config, ports[i]))
            {
              hyscan_sensor_control_set_enable (control, ports[i], FALSE);
              continue;
            }

          /* Включение порта. */
          if (!hyscan_sensor_control_set_enable (control, ports[i], TRUE))
            {
              g_message ("can't enable sensor port '%s'", ports[i]);
              goto exit;
            }

          /* Параметры порта. */
          channel = g_key_file_get_integer (config, ports[i], "channel", NULL);
          time_offset = g_key_file_get_int64 (config, ports[i], "time-offset", NULL);

          if (channel == 0)
            channel = 1;

          port_type = hyscan_sensor_control_get_port_type (control, ports[i]);
          uart_devices = hyscan_sensor_control_list_uart_devices (control, ports[i]);
          uart_modes = hyscan_sensor_control_list_uart_modes (control, ports[i]);
          ip_addresses = hyscan_sensor_control_list_ip_addresses (control, ports[i]);

          if (port_type == HYSCAN_SENSOR_PORT_VIRTUAL)
            {
              gboolean status;

              status = hyscan_sensor_control_set_virtual_port_param (control, ports[i],
                                                                     channel, time_offset);
              if (!status)
                {
                  g_message ("can't set sensor port '%s' parameters", ports[i]);
                  goto exit;
                }
            }

          else if ((port_type == HYSCAN_SENSOR_PORT_UART) && (uart_devices != NULL) && (uart_modes != NULL))
            {
              gchar *uart_device;
              gchar *uart_mode;
              guint uart_device_id;
              guint uart_mode_id;
              gboolean status;

              /* UART порт. */
              uart_device = g_key_file_get_string (config, ports[i], "uart-device", NULL);
              for (uart_device_id = 0, j = 0; uart_devices[j] != NULL; j++)
                {
                  if (g_strcmp0 (uart_device, uart_devices[j]->name) == 0)
                    {
                      uart_device_id = uart_devices[j]->value;
                      break;
                    }
                }
              if (uart_device_id == 0)
                {
                  g_message ("unknown uart device '%s' for sensor port '%s'", uart_device, ports[i]);
                  g_free (uart_device);
                  goto exit;
                }
              g_free (uart_device);

              /* Режим работы UART порта. */
              uart_mode = g_key_file_get_string (config, ports[i], "uart-mode", NULL);
              for (uart_mode_id = 0, j = 0; uart_modes[j] != NULL; j++)
                {
                  if (g_strcmp0 (uart_mode, uart_modes[j]->name) == 0)
                    {
                      uart_mode_id = uart_modes[j]->value;
                      break;
                    }
                }
              if (uart_mode_id == 0)
                {
                  g_message ("unknown uart mode '%s' for sensor port '%s'", uart_mode, ports[i]);
                  g_free (uart_mode);
                  goto exit;
                }
              g_free (uart_mode);

              status = hyscan_sensor_control_set_uart_port_param (control, ports[i],
                                                                  channel, time_offset,
                                                                  HYSCAN_SENSOR_PROTOCOL_NMEA_0183,
                                                                  uart_device_id, uart_mode_id);
              if (!status)
                {
                  g_message ("can't set sensor port '%s' parameters", ports[i]);
                  goto exit;
                }
            }

          else if ((port_type == HYSCAN_SENSOR_PORT_UDP_IP) && (ip_addresses != NULL))
            {
              gchar *ip_address;
              guint ip_address_id;
              guint udp_port;
              gboolean status;

              /* IP адрес. */
              ip_address = g_key_file_get_string (config, ports[i], "ip-address", NULL);
              for (ip_address_id = 0, j = 0; ip_addresses[j] != NULL; j++)
                {
                  if (g_strcmp0 (ip_address, ip_addresses[j]->name) == 0)
                    {
                      ip_address_id = ip_addresses[j]->value;
                      break;
                    }
                }
              if (ip_address_id == 0)
                {
                  g_message ("unknown ip address '%s' for sensor port '%s'", ip_address, ports[i]);
                  g_free (ip_address);
                  goto exit;
                }
              g_free (ip_address);

              /* UDP порт. */
              udp_port = g_key_file_get_integer (config, ports[i], "udp-port", NULL);
              if ((udp_port < 1024) || (udp_port > 65535))
                {
                  g_message ("udp port out of range for sensor port '%s'", ports[i]);
                  goto exit;
                }

              status = hyscan_sensor_control_set_udp_ip_port_param (control, ports[i],
                                                                    channel, time_offset,
                                                                    HYSCAN_SENSOR_PROTOCOL_NMEA_0183,
                                                                    ip_address_id, udp_port);
              if (!status)
                {
                  g_message ("can't set sensor port '%s' parameters", ports[i]);
                  goto exit;
                }
            }

          else
            {
              continue;
            }

          /* Местоположение антенны датчика. */
          position.x = g_key_file_get_double (config, ports[i], "position-x", NULL);
          position.y = g_key_file_get_double (config, ports[i], "position-y", NULL);
          position.z = g_key_file_get_double (config, ports[i], "position-z", NULL);
          position.psi = g_key_file_get_double (config, ports[i], "position-psi", NULL);
          position.gamma = g_key_file_get_double (config, ports[i], "position-gamma", NULL);
          position.theta = g_key_file_get_double (config, ports[i], "position-theta", NULL);
          position.psi *= (G_PI / 180.0);
          position.gamma *= (G_PI / 180.0);
          position.theta *= (G_PI / 180.0);
          if (!hyscan_sensor_control_set_position (control, ports[i], &position))
            {
              g_message ("can't set position for sensor connected to port '%s'", ports[i]);
              goto exit;
            }

          g_clear_pointer (&uart_devices, hyscan_data_schema_free_enum_values);
          g_clear_pointer (&uart_modes, hyscan_data_schema_free_enum_values);
          g_clear_pointer (&ip_addresses, hyscan_data_schema_free_enum_values);
        }
    }

  status = TRUE;

exit:
  g_strfreev (ports);

  return status;
}

gboolean
setup_sonar_antenna (HyScanSonarControl *control,
                     HyScanSourceType    source,
                     GKeyFile           *config)
{
  HyScanAntennaPosition position;
  const gchar *source_name;

  if (config == NULL)
    return TRUE;

  /* Местоположение приёмных гидроакустических антенн. */
  source_name = hyscan_channel_get_name_by_types (source, FALSE, 1);
  position.x = g_key_file_get_double (config, source_name, "position-x", NULL);
  position.y = g_key_file_get_double (config, source_name, "position-y", NULL);
  position.z = g_key_file_get_double (config, source_name, "position-z", NULL);
  position.psi = g_key_file_get_double (config, source_name, "position-psi", NULL);
  position.gamma = g_key_file_get_double (config, source_name, "position-gamma", NULL);
  position.theta = g_key_file_get_double (config, source_name, "position-theta", NULL);
  if (!hyscan_sonar_control_set_position (control, source, &position))
    {
      g_message ("can't set position for %s", source_name);
      return FALSE;
    }

  return TRUE;
}
