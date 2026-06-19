FROM gcc:13-bookworm AS build
WORKDIR /src
COPY Makefile ./
COPY src ./src
RUN make

FROM debian:bookworm-slim
RUN useradd -r -u 1000 httpd
WORKDIR /app
COPY --from=build /src/build/httpd /app/httpd
COPY www ./www
USER httpd
EXPOSE 8080
ENTRYPOINT ["/app/httpd", "-r", "/app/www"]
CMD ["-p", "8080"]
