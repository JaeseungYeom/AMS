#!/usr/bin/env python3

import ssl
import sys
import os
import json
import functools
import base64
import binascii
import re
import logging
import pika

# logging.basicConfig(level=logging.INFO)

def callback(ch, method, properties, body, args = None):
    print(
        f" [.] Received from exchange=\"{method.exchange}\" routing_key=\"{method.routing_key}\"\n"
        f"        > data   : \"{body}\"\n"
        f"        > args   : \"{args}\"")

def main(passwd: str = None, cacert: str = None, routing_key: str = ""):
    # # context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    # context = ssl.SSLContext(ssl.PROTOCOL_TLSv1_2)
    # context.verify_mode = ssl.CERT_REQUIRED
    # context.load_verify_locations("/g/g92/pottier1/ams/marbl-matprops-miniapp/docker/rabbitmq/certs/ca_certificate.pem")

    context = ssl.SSLContext(ssl.PROTOCOL_TLSv1_2)
    context.verify_mode = ssl.CERT_REQUIRED
    context.load_verify_locations("/g/g92/pottier1/ams/marbl-matprops-miniapp/docker/rabbitmq/certs/ca_certificate.pem")
    # context.load_cert_chain(
    #     certfile="/g/g92/pottier1/ams/marbl-matprops-miniapp/docker/rabbitmq/certs/client_lassen_certificate.pem",
    #     keyfile="/g/g92/pottier1/ams/marbl-matprops-miniapp/docker/rabbitmq/certs/client_lassen_key.pem"
    # )
 
    # context = ssl.create_default_context(
    #     cafile="/g/g92/pottier1/ams/marbl-matprops-miniapp/docker/rabbitmq/certs/ca_certificate.pem"
    # )
    # context.verify_mode = ssl.CERT_REQUIRED
    # context.load_cert_chain(
    #     certfile="/g/g92/pottier1/ams/marbl-matprops-miniapp/docker/rabbitmq/certs/client_lassen_certificate.pem",
    #     keyfile="/g/g92/pottier1/ams/marbl-matprops-miniapp/docker/rabbitmq/certs/client_lassen_key.pem"
    # )
    # ssl_options = pika.SSLOptions(context, "localhost")

    # ssl_options = ({
    #     "ca_certs": "/g/g92/pottier1/ams/marbl-matprops-miniapp/docker/rabbitmq/certs/ca_certificate.pem",
    #     "certfile": "/g/g92/pottier1/ams/marbl-matprops-miniapp/docker/rabbitmq/certs/client_certificate.pem",
    #     "keyfile": "/g/g92/pottier1/ams/marbl-matprops-miniapp/docker/rabbitmq/certs/client_key.pem",
    #     "cert_reqs": ssl.CERT_REQUIRED,
    #     "server_side": False
    # })

    credentials = pika.PlainCredentials(
        "ams-user",
        "ZhjN5ZSw5aeanFyPZ4QZE9JBl-8TeBmmG9gcvRg_PWUox2cmbZVdfccRxpfwF6s12fztrEwSHgQgxvqF"
    )
    cp = pika.ConnectionParameters(
        host="localhost",
        port=5671,
        virtual_host="/",
        credentials=credentials,
        ssl_options=pika.SSLOptions(context)
    )

    # cp = pika.ConnectionParameters(
    #     host="localhost",
    #     port=5671,
    #     virtual_host="/",
    #     credentials=pika.ExternalCredentials(),
    #     ssl=True,
    #     ssl_options=ssl_options
    # )

    connection = pika.BlockingConnection(cp)
    channel = connection.channel()

    print(f"[recv.py] Connecting to RMQ ...")

    # Warning:
    #   if no queue is specified then RabbitMQ will NOT hold messages that are not routed to queues.
    #   So in order to receive the message, the receiver will have to be started BEFORE the sender
    #   Otherwise the message will be lost.

    result = channel.queue_declare(queue=routing_key, exclusive=False)
    queue_name = result.method.queue
    channel.basic_consume(queue=queue_name, on_message_callback=callback, auto_ack=True)
    print(f"[recv.py] Listening on queue = {queue_name}")

    print(" [*] Waiting for messages. To exit press CTRL+C")
    channel.start_consuming()

if __name__ == "__main__":
    try:
        # main(passwd = sys.argv[3], cacert = sys.argv[1], routing_key = sys.argv[2])
        main()
    except KeyboardInterrupt:
        print("")
        print("Interrupted")
        try:
            sys.exit(0)
        except SystemExit:
            os._exit(0)
