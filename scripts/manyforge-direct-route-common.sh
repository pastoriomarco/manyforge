#!/usr/bin/env bash
# Shared helpers for the dedicated ManyForge TLS route.

if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
    echo "This file is meant to be sourced, not executed directly." >&2
    exit 1
fi

manyforge_direct_route_dir() {
    printf '%s\n' "${MANYFORGE_DIRECT_ROUTE_DIR:-$HOME/.manyforge/direct-route}"
}

manyforge_direct_route_port() {
    printf '%s\n' "${MANYFORGE_DIRECT_ROUTE_PORT:-443}"
}

manyforge_direct_route_ca_cert_path() {
    printf '%s/ca-cert.pem\n' "$(manyforge_direct_route_dir)"
}

manyforge_direct_route_ca_key_path() {
    printf '%s/ca-key.pem\n' "$(manyforge_direct_route_dir)"
}

manyforge_direct_route_server_cert_path() {
    printf '%s/server-cert.pem\n' "$(manyforge_direct_route_dir)"
}

manyforge_direct_route_server_key_path() {
    printf '%s/server-key.pem\n' "$(manyforge_direct_route_dir)"
}

manyforge_direct_route_token_path() {
    printf '%s/plugin-token.txt\n' "$(manyforge_direct_route_dir)"
}

manyforge_direct_route_nginx_conf_path() {
    printf '%s/nginx.conf\n' "$(manyforge_direct_route_dir)"
}

manyforge_direct_route_log_path() {
    printf '%s/nginx.log\n' "$(manyforge_direct_route_dir)"
}

manyforge_direct_route_container_name() {
    printf '%s\n' "${MANYFORGE_DIRECT_ROUTE_CONTAINER_NAME:-manyforge-direct-route}"
}

manyforge_direct_route_image() {
    printf '%s\n' "${MANYFORGE_DIRECT_ROUTE_IMAGE:-nginx:1.27-alpine}"
}

manyforge_direct_route_proxy_is_running() {
    local name
    name="$(manyforge_direct_route_container_name)"
    docker ps --filter "name=^/${name}$" --format '{{.Names}}' 2>/dev/null | grep -Fx "${name}" >/dev/null 2>&1
}

ensure_manyforge_direct_route_artifacts() {
    local route_dir ca_cert ca_key server_cert server_key token_path ext_file csr_file
    route_dir="$(manyforge_direct_route_dir)"
    ca_cert="$(manyforge_direct_route_ca_cert_path)"
    ca_key="$(manyforge_direct_route_ca_key_path)"
    server_cert="$(manyforge_direct_route_server_cert_path)"
    server_key="$(manyforge_direct_route_server_key_path)"
    token_path="$(manyforge_direct_route_token_path)"

    mkdir -p "${route_dir}"

    if ! command -v openssl >/dev/null 2>&1; then
        echo "ERROR: openssl is required to provision the ManyForge direct route certs" >&2
        return 1
    fi

    if [[ ! -s "${token_path}" ]]; then
        python3 - <<'PY' > "${token_path}"
import secrets
print(secrets.token_urlsafe(32))
PY
        chmod 600 "${token_path}"
    fi

    if [[ ! -s "${ca_cert}" || ! -s "${ca_key}" ]]; then
        openssl req -x509 -newkey rsa:2048 -sha256 -days 825 -nodes \
            -keyout "${ca_key}" \
            -out "${ca_cert}" \
            -subj "/CN=ManyForge Local Route CA" >/dev/null 2>&1
        chmod 600 "${ca_key}"
    fi

    if [[ ! -s "${server_cert}" || ! -s "${server_key}" ]]; then
        ext_file="${route_dir}/server-ext.cnf"
        csr_file="${route_dir}/server.csr"
        cat > "${ext_file}" <<'EOF'
subjectAltName=DNS:host.openshell.internal,DNS:localhost,IP:127.0.0.1
extendedKeyUsage=serverAuth
basicConstraints=CA:FALSE
keyUsage=digitalSignature,keyEncipherment
EOF
        openssl req -new -newkey rsa:2048 -nodes \
            -keyout "${server_key}" \
            -out "${csr_file}" \
            -subj "/CN=host.openshell.internal" >/dev/null 2>&1
        openssl x509 -req -in "${csr_file}" \
            -CA "${ca_cert}" \
            -CAkey "${ca_key}" \
            -CAcreateserial \
            -out "${server_cert}" \
            -days 397 -sha256 \
            -extfile "${ext_file}" >/dev/null 2>&1
        rm -f "${csr_file}" "${ext_file}"
        chmod 600 "${server_key}"
    fi

    export MANYFORGE_DIRECT_ROUTE_TOKEN
    MANYFORGE_DIRECT_ROUTE_TOKEN="$(tr -d '\n' < "${token_path}")"
}

render_manyforge_direct_route_nginx_conf() {
    local route_dir config_path route_token upstream_token
    route_dir="$(manyforge_direct_route_dir)"
    config_path="$(manyforge_direct_route_nginx_conf_path)"
    route_token="$(tr -d '\n' < "$(manyforge_direct_route_token_path)")"
    upstream_token="${MANYFORGE_API_TOKEN:-}"

    cat > "${config_path}" <<EOF
worker_processes auto;
error_log /dev/stderr warn;
pid /tmp/nginx.pid;

events {
    worker_connections 1024;
}

http {
    access_log /dev/stdout;
    sendfile on;
    tcp_nopush on;
    keepalive_timeout 65;

    map \$http_authorization \$manyforge_authorized {
        default 0;
        "~^Bearer ${route_token}\$" 1;
    }

    server {
        listen 443 ssl;
        server_name host.openshell.internal localhost;

        ssl_certificate     /etc/manyforge-direct/server-cert.pem;
        ssl_certificate_key /etc/manyforge-direct/server-key.pem;
        ssl_protocols TLSv1.2 TLSv1.3;
        ssl_session_timeout 1d;
        ssl_session_cache shared:SSL:10m;
        ssl_prefer_server_ciphers off;

        proxy_http_version 1.1;
        proxy_buffering off;
        proxy_request_buffering on;
        proxy_read_timeout 300s;
        proxy_send_timeout 300s;

        add_header X-Accel-Buffering no always;

        if (\$manyforge_authorized = 0) {
            return 401;
        }

        location = /api/health {
            if (\$request_method !~ ^(GET|HEAD)\$) { return 405; }
            proxy_set_header Host 127.0.0.1:9000;
            proxy_set_header X-Principal-ID \$http_x_principal_id;
EOF

    if [[ -n "${upstream_token}" ]]; then
        cat >> "${config_path}" <<EOF
            proxy_set_header Authorization "Bearer ${upstream_token}";
EOF
    fi

    cat >> "${config_path}" <<'EOF'
            proxy_pass http://127.0.0.1:9000/api/health;
        }

        location = /api/infra/status {
            if ($request_method !~ ^(GET|HEAD)$) { return 405; }
            proxy_set_header Host 127.0.0.1:9000;
            proxy_set_header X-Principal-ID $http_x_principal_id;
EOF

    if [[ -n "${upstream_token}" ]]; then
        cat >> "${config_path}" <<EOF
            proxy_set_header Authorization "Bearer ${upstream_token}";
EOF
    fi

    cat >> "${config_path}" <<'EOF'
            proxy_pass http://127.0.0.1:9000/api/infra/status;
        }

        location = /api/mcp {
            if ($request_method !~ ^(POST|OPTIONS)$) { return 405; }
            proxy_set_header Host 127.0.0.1:9000;
            proxy_set_header X-Principal-ID $http_x_principal_id;
            proxy_set_header Content-Type $http_content_type;
EOF

    if [[ -n "${upstream_token}" ]]; then
        cat >> "${config_path}" <<EOF
            proxy_set_header Authorization "Bearer ${upstream_token}";
EOF
    fi

    cat >> "${config_path}" <<'EOF'
            proxy_pass http://127.0.0.1:9000/api/mcp;
        }

        location = /api/mcp/sse {
            if ($request_method !~ ^(GET|OPTIONS)$) { return 405; }
            proxy_set_header Host 127.0.0.1:9000;
            proxy_set_header X-Principal-ID $http_x_principal_id;
EOF

    if [[ -n "${upstream_token}" ]]; then
        cat >> "${config_path}" <<EOF
            proxy_set_header Authorization "Bearer ${upstream_token}";
EOF
    fi

    cat >> "${config_path}" <<'EOF'
            proxy_pass http://127.0.0.1:9000/api/mcp/sse;
        }

        location / {
            return 404;
        }
    }
}
EOF
}

start_manyforge_direct_route_proxy() {
    local route_dir config_path log_file container_name image
    ensure_manyforge_direct_route_artifacts || return 1
    render_manyforge_direct_route_nginx_conf || return 1

    route_dir="$(manyforge_direct_route_dir)"
    config_path="$(manyforge_direct_route_nginx_conf_path)"
    log_file="$(manyforge_direct_route_log_path)"
    container_name="$(manyforge_direct_route_container_name)"
    image="$(manyforge_direct_route_image)"

    if ! command -v docker >/dev/null 2>&1; then
        echo "ERROR: docker is required to run the ManyForge direct TLS route" >&2
        return 1
    fi

    docker rm -f "${container_name}" >/dev/null 2>&1 || true
    docker run -d \
        --name "${container_name}" \
        --network host \
        -v "${config_path}:/etc/nginx/nginx.conf:ro" \
        -v "${route_dir}:/etc/manyforge-direct:ro" \
        "${image}" \
        >/dev/null 2>&1 || return 1

    sleep 2
    docker logs "${container_name}" > "${log_file}" 2>&1 || true

    manyforge_direct_route_proxy_is_running
}

stop_manyforge_direct_route_proxy() {
    docker rm -f "$(manyforge_direct_route_container_name)" >/dev/null 2>&1 || true
}
