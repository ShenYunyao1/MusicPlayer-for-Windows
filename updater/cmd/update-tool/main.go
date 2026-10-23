// Build-time signing tool. Never included in the end-user ZIP.
package main

import (
	"crypto/ed25519"
	"crypto/rand"
	"crypto/x509"
	"encoding/pem"
	"flag"
	"fmt"
	"musicplayer/updater/internal/update"
	"os"
	"path/filepath"
)

func main() {
	mode := flag.String("mode", "verify", "keygen, sign or verify")
	dir := flag.String("directory", ".", "release directory or key output directory")
	key := flag.String("key", "", "PEM key file (signing alternatively uses CLIENT_UPDATE_SIGNING_KEY)")
	flag.Parse()
	if err := run(*mode, *dir, *key); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
func run(mode, dir, key string) error {
	if mode == "keygen" {
		pub, priv, e := ed25519.GenerateKey(rand.Reader)
		if e != nil {
			return e
		}
		if e = os.MkdirAll(dir, 0700); e != nil {
			return e
		}
		private, _ := x509.MarshalPKCS8PrivateKey(priv)
		public, _ := x509.MarshalPKIXPublicKey(pub)
		for name, block := range map[string]*pem.Block{"private.pem": {Type: "PRIVATE KEY", Bytes: private}, "public.pem": {Type: "PUBLIC KEY", Bytes: public}} {
			f, e := os.OpenFile(filepath.Join(dir, name), os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0600)
			if e != nil {
				return e
			}
			_, e = f.Write(pem.EncodeToMemory(block))
			f.Close()
			if e != nil {
				return e
			}
		}
		fmt.Println("Created dedicated client key pair (private key not displayed)")
		return nil
	}
	raw, e := os.ReadFile(filepath.Join(dir, "client-release-manifest.json"))
	if e != nil {
		return e
	}
	b := []byte(os.Getenv("CLIENT_UPDATE_SIGNING_KEY"))
	if key != "" {
		b, e = os.ReadFile(key)
		if e != nil {
			return e
		}
	}
	if mode == "sign" {
		block, _ := pem.Decode(b)
		if block == nil {
			return fmt.Errorf("missing PEM client signing key")
		}
		v, e := x509.ParsePKCS8PrivateKey(block.Bytes)
		if e != nil {
			return e
		}
		priv, ok := v.(ed25519.PrivateKey)
		if !ok {
			return fmt.Errorf("not Ed25519")
		}
		sig := ed25519.Sign(priv, raw)
		if _, e = update.ParseManifest(raw, sig, priv.Public().(ed25519.PublicKey)); e != nil {
			return e
		}
		return os.WriteFile(filepath.Join(dir, "client-release-manifest.json.sig"), sig, 0600)
	}
	if mode != "verify" {
		return fmt.Errorf("unknown mode")
	}
	block, _ := pem.Decode(b)
	if block == nil {
		return fmt.Errorf("missing public PEM key")
	}
	v, e := x509.ParsePKIXPublicKey(block.Bytes)
	if e != nil {
		return e
	}
	pub, ok := v.(ed25519.PublicKey)
	if !ok {
		return fmt.Errorf("not Ed25519")
	}
	sig, e := os.ReadFile(filepath.Join(dir, "client-release-manifest.json.sig"))
	if e != nil {
		return e
	}
	m, e := update.ParseManifest(raw, sig, pub)
	if e != nil {
		return e
	}
	zip, e := os.ReadFile(filepath.Join(dir, "MusicPlayer2.zip"))
	if e != nil {
		return e
	}
	_, e = update.ReadPackage(zip, m)
	if e == nil {
		fmt.Println("Signed package verified:", m.Version, m.Commit)
	}
	return e
}
