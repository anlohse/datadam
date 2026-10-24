# syntax=docker/dockerfile:1
# The tools the cloud test runs from: AWS CLI v2, Terraform, the Session
# Manager plugin and Python, so nothing has to be installed on the host.
# Built and run by cloud/tools.sh.
FROM amazon/aws-cli:2.22.35

# Optional extra root CA for networks/antivirus that intercept HTTPS:
#   docker build --secret id=extra_ca,src=my-root-ca.crt ...
RUN --mount=type=secret,id=extra_ca \
    if [ -s /run/secrets/extra_ca ]; then \
      cp /run/secrets/extra_ca /etc/pki/ca-trust/source/anchors/extra-ca.crt && update-ca-trust; \
    fi

ARG TERRAFORM_VERSION=1.9.8
RUN yum install -y -q unzip python3 \
    && curl -fsSLo /tmp/terraform.zip \
       "https://releases.hashicorp.com/terraform/${TERRAFORM_VERSION}/terraform_${TERRAFORM_VERSION}_linux_amd64.zip" \
    && unzip -q /tmp/terraform.zip -d /usr/local/bin && rm /tmp/terraform.zip \
    && yum install -y -q https://s3.amazonaws.com/session-manager-downloads/plugin/latest/linux_64bit/session-manager-plugin.rpm \
    && yum clean all

COPY entry.sh /usr/local/bin/entry.sh
WORKDIR /work
ENTRYPOINT ["/usr/local/bin/entry.sh"]
