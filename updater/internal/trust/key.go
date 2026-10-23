package trust

import (
	"crypto/ed25519"
	"crypto/x509"
	_ "embed"
	"encoding/pem"
)

//go:embed public.pem
var PublicPEM []byte

func PublicKey() ed25519.PublicKey {
	b, _ := pem.Decode(PublicPEM)
	if b == nil {
		panic("invalid embedded public key")
	}
	k, e := x509.ParsePKIXPublicKey(b.Bytes)
	if e != nil {
		panic(e)
	}
	return k.(ed25519.PublicKey)
}
