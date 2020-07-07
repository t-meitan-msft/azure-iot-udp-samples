# Introduction 

## Problem statement
Commonly, Wireless Sensor Networks (WSN) use the MQTT protocol over a TCP/IP transport layer since TCP/IP can provide reliability to the unreliable nature of wireless links. In Azure IoT’s context, various IoT solution developers are concerned about the data usage of their IoT devices in cellular networks. For example, communicating over LTE network could get expensive either over time or as the solution scales larger. Moreover, as the endpoint devices are often resource constrained, it is preferred to optimize for a more light-weight protocol stack, like UDP, which consumes less network bandwidth compared to TCP/IP with its connection keepalive packets and other packets that ensures delivery. Finally, since most of the devices are battery powered, we would also prefer a less computationally intensive stack, in addition to transmitting less packets wirelessly, to extend battery life.

## Solution
New MQTT-like protocols run over other transports like UDP, LoRa, ZigBee, Bluetooth have been offering a solution to reduce data usage and power consumption, while using gateways to easily integrate with existing infrastructure such as the Azure IoT Hub MQTT Broker. We will first look at MQTT-SN, which can run on UDP and other transports, and then at MQTT/UDP, which runs on UDP. Most of these new MQTT-like protocols have broadcasting, discovery, and publish-subscribe functionalities to adapt to dynamic systems where devices come in and out of range of each other. 

## Goal
We would like to try the new MQTT over UDP protocols to evaluate their efficiency improvement when communicating with the Azure IoT Hub MQTT broker endpoint, as compared to the traditional MQTT over TCP/IP. Specifically, we are interested in the reduction of data usage, network traffic, and power usage, especially when introducing packet loss into the network. These metrics will ultimately reduce LTE network costs, for example, for our IoT partners.

## Implementation 
For our purposes, we will be evaluating two major implementations of MQTT over the UDP transport: [MQTT-SN](http://mqtt.org/new/wp-content/uploads/2009/06/MQTT-SN_spec_v1.2.pdf) and [MQTT/UDP](https://github.com/dzavalishin/mqtt_udp/wiki/MQTT-UDP-protocol-specification). In both cases, we would need a Gateway to translate the new MQTT packets to classic MQTT that the Azure IoT Hub’s MQTT broker understands. 

# Getting Started
Instructions to configure and run a MQTT-SN Gateway and Client can be found in [this guide](samples\MQTTSN\README.md).

<!-- TODO: Instructions to configure and run a MQTT/UDP Gateway and Client can be found in [this guide](samples\MQTTSN\README.md). -->

# Testing Plan

## Data usage
We will be comparing the data usage between the Client and the Gateway for both MQTT-SN and MQTT/UDP with the data usage between the Client and the Azure IoT Hub Broker for the classic MQTT over TCP/IP. We will initially measure and compare each protocol’s performance under lossless conditions. Then, we will introduce some packet loss (with [traffic shaper tc](https://www.badunetworks.com/traffic-shaping-with-tc/)) in the network to measure the increased overhead from each protocol’s attempt at ensuring reliability. 

## Power usage
We will be measuring the overall power usage (with measuring the change in the battery’s voltage levels) of both MQTT-SN and MQTT/UDP against the traditional MQTT over TCP/IP in a similar methodology as comparing their data usage. We can use [Powertop](https://wiki.archlinux.org/index.php/Powertop) for Linux environments or implement the Clients on actual battery powered devices and measure the voltage level with a multimeter. 

# Resources

## MQTT-SN

* [Introduction to MQTT-SN](http://www.steves-internet-guide.com/mqtt-sn/) (Can watch the video for quick overview)

* [RSMB and Python Client Demo](http://www.steves-internet-guide.com/mqtt-sn-rsmb-install/) (Can watch the video)

* [Ian Craggs Presentation on MQTT-SN](https://www.infoq.com/presentations/mqtt-sn/#downloadPdf/) (Can watch video and download the slides)

* [Library code base](https://github.com/eclipse/paho.mqtt-sn.embedded-c)

* [Protocol Specification](http://mqtt.org/new/wp-content/uploads/2009/06/MQTT-SN_spec_v1.2.pdf)


## MQTT/UDP 

* [Docs about MQTT/UDP](https://mqtt-udp.readthedocs.io/en/latest/#welcome-to-mqtt-udp)

* [Wiki about MQTT/UDP](https://github.com/dzavalishin/mqtt_udp/wiki/MQTT-UDP-protocol-specification)

* [Library code base](https://github.com/dzavalishin/mqtt_udp)

* [Protocol Specification](https://github.com/dzavalishin/mqtt_udp/wiki/MQTT-UDP-protocol-specification)

## Other

* [UDP + IoT Hub](https://www.danielemaggio.eu/iot/udp-iot-edge/)