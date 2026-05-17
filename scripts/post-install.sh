#!/bin/bash

getent passwd agentos > /dev/null || useradd \
  --system \
  --no-create-home \
  --shell /usr/sbin/nologin \
  --comment "AgentOS daemon" \
  agentos
