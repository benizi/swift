# ubuntu:15.10 at some point in time:
FROM ubuntu@sha256:ae24faeb7d968197008eb7fa6970d1aa90636963947fe3486af27b079cccfb17

RUN gpg --keyserver hkp://pool.sks-keyservers.net \
  --recv-keys \
  '7463 A81A 4B2E EA1B 551F FBCF D441 C977 412B 37AD' \
  '1BE1 E29A 084C B305 F397 D62A 9F59 7F4D 21A5 6D5F'

RUN apt-get update && apt-get install -y \
  curl clang libicu-dev libedit-dev python2.7-dev

RUN curl -so /targz https://swift.org/builds/ubuntu1510/swift-2.2-SNAPSHOT-2015-12-01-b/swift-2.2-SNAPSHOT-2015-12-01-b-ubuntu15.10.tar.gz && \
  curl -so /targz.sig https://swift.org/builds/ubuntu1510/swift-2.2-SNAPSHOT-2015-12-01-b/swift-2.2-SNAPSHOT-2015-12-01-b-ubuntu15.10.tar.gz.sig && \
  gpg --verify /targz.sig && \
  mkdir /swift && \
  tar --strip-components=2 -C /swift -xz < /targz && \
  rm /targz

WORKDIR /src
ENV PATH="/swift/bin:$PATH"
CMD bash

ADD stringtest.swift /
ADD test /
