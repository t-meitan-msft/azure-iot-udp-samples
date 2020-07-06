## Clone this repository
```
git clone https://github.com/t-meitan-msft/azure-iot-udp-samples.git

git submodule update --init --recursive
```

## Build the Paho MQTT-SN Client Sample

In a Linux environment, run:

```
cd azure-iot-udp-samples/samples/MQTTSN

mkdir client_build

cd client_build

cmake ..

cmake --build . 
```
---

> **NOTE** that configuring, building, and running the Paho MQTT-SN Gateway can be done independently from this sample, by cloning the Eclipse Paho MQTT-SN Repository and working in that directory. 

## Build the Paho MQTT-SN Gateway

> Bug #1 (?): gateway.conf should be same folder as MQTT-SNGateway.exe

> Bug #2: modify in [MQTTSNProcess.h](samples\MQTTSN\lib\paho.mqtt-sn.embedded-c\MQTTSNGateway\src\MQTTSNGWProcess.h) the value of *MQTTSNGW_PARAM_MAX* to be more than the size of the gateway parameters size (for example, our Password parameter exceeds 128 so we changed to 256).

```
cd azure-iot-udp-samples/samples/MQTTSN/lib/paho.mqtt-sn.embedded-c/MQTTSNGateway

make

make install INSTALL_D
IR=<path to your gateway_build folder> CONFIG_DIR=<same path as gateway_build folder>

make clean
```

## Configure the Paho MQTT-SN Gateway

Edit the configuration files according to [the Paho MQTTSNGateway guide](lib\paho.mqtt-sn.embedded-c\MQTTSNGateway\README.md). 

For our purposes, we have modified the following parameters:

#### In _gateway.conf_:

```
BrokerName = <your hub name>.azure-devices.net

ClientAuthentication = YES

ClientsList = <path to your>/clients.conf

RootCAfile = <path to your /CAfile.crt certificate>

RootCApath = <path to your /certs/ folder>
```

> Configure the following two parameters according to [the Azure IoT Hub MQTT Support guide](https://docs.microsoft.com/en-us/azure/iot-hub/iot-hub-mqtt-support).
* LoginID
* Password (if device authenticates using SaS symmetric key)

> If device is Self-signed or CA signed, use the following parameters:

```
CertsFile = <path to your /device_cert.pem>

PrivateKey = <path to your /device_private_key.pem>
```

#### In _clients.conf_:

> We use _secureConnection_ since Azure IoT Hub is using TLS.

`<deviceID>,<deviceIP>:<devicePort>,secureConnection`

## Run the Gateway 

```
cd <path to your gateway build folder>

./MQTT-SNGateway -f gateway.conf
```

## Monitoring the packets on Azure IoT Hub (optional)

* Open [Azure IoT Explorer](https://github.com/Azure/azure-iot-explorer/releases)
* Connect to the Azure IoT Hub and go to Telemetry 
* Start listening to telemetry events

## Run the Device Sample

Set the following environment variables for the device

```
export AZ_IOT_DEVICE_ID=<your device ID>

export AZ_IOT_HUB_HOSTNAME=<your hub name>.azure-devices.net
```

Can connect to the default gateway address and port number specified in the sample or can optionally specify a different address and port:

```
cd <path to your local repo>/azure-iot-udp-samples/samples/MQTTSN/client_build

./sample_qos_0 [host address] [destination port]

or

./sample_qos_1 [host address] [destination port]
```
